#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <stdint.h>

#include <elliptics/session.hpp>

#include <msgpack.hpp>

#include <boost/program_options.hpp>

#include <ribosome/timer.hpp>

static inline std::string lexical_cast(size_t value) {
	if (value == 0) {
		return std::string("0");
	}

	std::string result;
	size_t length = 0;
	size_t calculated = value;
	while (calculated) {
		calculated /= 10;
		++length;
	}

	result.resize(length);
	while (value) {
		--length;
		result[length] = '0' + (value % 10);
		value /= 10;
	}

	return result;
}

namespace ioremap { namespace indexes {
static size_t max_page_size = 4096;

#define dprintf(fmt, a...) do {} while (0)
//#define dprintf(fmt, a...) printf(fmt, ##a)

struct eurl {
	std::string bucket;
	std::string key;

	MSGPACK_DEFINE(bucket, key);


	size_t size() const {
		return bucket.size() + key.size();
	}

	std::string str() const {
		return bucket + "/" + key;
	}

	bool operator!=(const eurl &other) const {
		return bucket != other.bucket || key != other.key;
	}

	bool operator==(const eurl &other) const {
		return bucket == other.bucket && key == other.key;
	}

	bool operator<(const eurl &other) const {
		return bucket < other.bucket || key < other.key;
	}
	bool operator<=(const eurl &other) const {
		return bucket <= other.bucket || key <= other.key;
	}

	bool empty() const {
		return key.empty();
	}
};

struct key {
	std::string id;
	eurl url;

	size_t size() const {
		return id.size() + url.size();
	}

	bool operator<(const key &other) const {
		return id < other.id;
	}
	bool operator<=(const key &other) const {
		return id <= other.id;
	}
	bool operator==(const key &other) const {
		return id == other.id;
	}
	bool operator!=(const key &other) const {
		return id != other.id;
	}

	operator bool() const {
		return id.size() != 0;
	}
	bool operator !() const {
		return !operator bool();
	}

	std::string str() const {
		return id + ":" + url.str();
	}

	MSGPACK_DEFINE(id, url);
};

#define PAGE_LEAF		(1<<0)

struct page {
	uint32_t flags = 0;
	std::vector<key> objects;
	size_t total_size = 0;
	eurl next;

	MSGPACK_DEFINE(flags, objects, total_size, next);

	page(bool leaf = false) {
		if (leaf) {
			flags = PAGE_LEAF;
		}
	}

	bool is_empty() const {
		return objects.size() == 0;
	}

	bool operator==(const page &other) const {
		return ((flags == other.flags) && (objects == other.objects));
	}
	bool operator!=(const page &other) const {
		return ((flags != other.flags) || (objects != other.objects));
	}

	std::string str() const {
		std::ostringstream ss;
		if (objects.size() > 0) {
			ss << "[" << objects.front().str() <<
				", " << objects.back().str() <<
				", L" << (is_leaf() ? 1 : 0) <<
				", N" << objects.size() <<
				", T" << total_size <<
				")";
		} else {
			ss << "[" << 
				"L" << (is_leaf() ? 1 : 0) <<
				", N" << objects.size() <<
				", T" << total_size <<
				")";
		}
		return ss.str();
	}

	bool is_leaf() const {
		return flags & PAGE_LEAF;
	}

	void load(const void *data, size_t size) {
		objects.clear();
		flags = 0;
		next = eurl();
		total_size = 0;

		msgpack::unpacked result;

		msgpack::unpack(&result, (const char *)data, size);
		msgpack::object obj = result.get();
		obj.convert(this);

		dprintf("page load: %s\n", str().c_str());
	}

	std::string save() const {
		std::stringstream ss;
		msgpack::pack(ss, *this);

		dprintf("page save: %s\n", str().c_str());

		return ss.str();
	}

	// return position of the given key in @objects vector
	int search_leaf(const key &obj) const {
		if (!is_leaf()) {
			return -1;
		}

		auto it = std::lower_bound(objects.begin(), objects.end(), obj);
		if (it == objects.end())
			return -1;

		if (*it != obj)
			return -1;

		return it - objects.begin();
	}

	// returns position of the key in @objects vector
	int search_node(const key &obj) const {
		if (objects.size() == 0)
			return -1;

		if (is_leaf()) {
			return search_leaf(obj);
		}

		if (obj <= objects.front()) {
			return 0;
		}

		auto it = std::lower_bound(objects.begin(), objects.end(), obj);
		if (it == objects.end()) {
			return objects.size() - 1;
		}

		if (*it == obj)
			return it - objects.begin();

		return (it - objects.begin()) - 1;
	}

	// returnes true if modified page is subject to compaction
	bool remove(size_t remove_pos) {
		total_size -= objects[remove_pos].size();
		for (size_t pos = remove_pos + 1, objects_num = objects.size(); pos < objects_num; ++pos) {
			objects[pos - 1] = objects[pos];
		}

		objects.resize(objects.size() - 1);


		return total_size < max_page_size / 3;
	}

	bool insert_and_split(const key &obj, page &other) {
		std::vector<key> copy;
		bool copied = false;

		for (auto it = objects.begin(); it != objects.end(); ++it) {
			if (obj <= *it) {
				copy.push_back(obj);
				total_size += obj.size();
				copied = true;

				if (obj == *it) {
					total_size -= it->size();
					++it;
				}

				copy.insert(copy.end(), it, objects.end());
				break;
			}

			copy.push_back(*it);
		}

		if (!copied) {
			copy.push_back(obj);
			total_size += obj.size();
		}

		if (total_size > max_page_size) {
			size_t split_idx = copy.size() / 2;

			other.flags = flags;
			other.objects = std::vector<key>(std::make_move_iterator(copy.begin() + split_idx),
							 std::make_move_iterator(copy.end()));
			other.recalculate_size();

			copy.erase(copy.begin() + split_idx, copy.end());
			objects.swap(copy);
			recalculate_size();

			dprintf("insert/split: %s: split: %s %s\n", obj.str().c_str(), str().c_str(), other.str().c_str());

			return true;
		}

		objects.swap(copy);

		dprintf("insert/split: %s: %s\n", obj.str().c_str(), str().c_str());
		return false;
	}

	void recalculate_size() {
		total_size = 0;
		for_each(objects.begin(), objects.end(), [&] (const key &obj)
				{
					total_size += obj.size();
				});
	}
};

template <typename T>
class page_iterator {
public:
	typedef page_iterator self_type;
	typedef page value_type;
	typedef page& reference;
	typedef page* pointer;
	typedef std::forward_iterator_tag iterator_category;
	typedef std::ptrdiff_t difference_type;

	page_iterator(T &t, const page &p) : m_t(t), m_page(p) {}
	page_iterator(T &t, const eurl &url) : m_t(t), m_url(url) {
		elliptics::read_result_entry e = m_t.read(url);
		if (e.error())
			return;

		m_page.load(e.file().data(), e.file().size());
	}
	page_iterator(const page_iterator &i) : m_t(i.m_t) {
		m_page = i.m_page;
		m_page_index = i.m_page_index;
	}

	self_type operator++() {
		try_loading_next_page();

		return *this;
	}

	self_type operator++(int num) {
		try_loading_next_page();

		return *this;
	}

	reference operator*() {
		return m_page;
	}
	pointer operator->() {
		return &m_page;
	}

	bool operator==(const self_type& rhs) {
		return m_page == rhs.m_page;
	}
	bool operator!=(const self_type& rhs) {
		return m_page != rhs.m_page;
	}

	eurl url() const {
		return m_url;
	}

private:
	T &m_t;
	page m_page;
	size_t m_page_index = 0;
	eurl m_url;

	void try_loading_next_page() {
		++m_page_index;

		if (m_page.next.empty()) {
			m_page = page();
			m_url = eurl();
		} else {
			m_url = m_page.next;
			elliptics::read_result_entry e = m_t.read(m_url);
			if (e.error()) {
				m_page = page();
				return;
			}
			m_page.load(e.file().data(), e.file().size());
		}
	}
};

template <typename T>
class iterator {
public:
	typedef iterator self_type;
	typedef key value_type;
	typedef key& reference;
	typedef key* pointer;
	typedef std::forward_iterator_tag iterator_category;
	typedef std::ptrdiff_t difference_type;

	iterator(T &t, page &p, size_t internal_index) : m_t(t), m_page(p), m_page_internal_index(internal_index) {}
	iterator(const iterator &i) : m_t(i.m_t) {
		m_page = i.m_page;
		m_page_internal_index = i.m_page_internal_index;
		m_page_index = i.m_page_index;
	}

	self_type operator++() {
		++m_page_internal_index;
		try_loading_next_page();

		return *this;
	}

	self_type operator++(int num) {
		m_page_internal_index += num;
		try_loading_next_page();

		return *this;
	}

	reference operator*() {
		return m_page.objects[m_page_internal_index];
	}
	pointer operator->() {
		return &m_page.objects[m_page_internal_index];
	}

	bool operator==(const self_type& rhs) {
		return (m_page == rhs.m_page) && (m_page_internal_index == rhs.m_page_internal_index);
	}
	bool operator!=(const self_type& rhs) {
		return (m_page != rhs.m_page) || (m_page_internal_index != rhs.m_page_internal_index);
	}
private:
	T &m_t;
	page m_page;
	size_t m_page_index = 0;
	size_t m_page_internal_index = 0;

	void try_loading_next_page() {
		if (m_page_internal_index >= m_page.objects.size()) {
			m_page_internal_index = 0;
			++m_page_index;

			if (m_page.next.empty()) {
				m_page = page();
			} else {
				elliptics::read_result_entry e = m_t.read(m_page.next);
				if (e.error()) {
					m_page = page();
					return;
				}
				m_page.load(e.file().data(), e.file().size());
			}
		}
	}
};

struct index_meta {
	uint64_t page_index = 0;
	uint64_t num_pages = 0;
	uint64_t num_leaf_pages = 0;
	uint64_t generation_number = 0;

	MSGPACK_DEFINE(page_index, num_pages, num_leaf_pages, generation_number);

	bool operator != (const index_meta &other) const {
		return ((page_index != other.page_index) ||
				(num_pages != other.num_pages) ||
				(num_leaf_pages != other.num_leaf_pages) ||
				(generation_number != other.generation_number)
			);
	}

	std::string str() const {
		std::ostringstream ss;
		ss << "page_index: " << page_index <<
			", num_pages: " << num_pages <<
			", num_leaf_pages: " << num_leaf_pages <<
			", generation_number: " << generation_number;
		return ss.str();
	}
};

struct recursion {
	key page_start;
	key split_key;
};

struct remove_recursion {
	key page_start;
	bool removed = false;
};

template <typename T>
class index {
public:
	index(T &t, const eurl &sk): m_t(t), m_sk(sk) {
		std::vector<elliptics::read_result_entry> meta = m_t.read_all(meta_key());

		struct separate_index_meta {
			int group = 0;
			index_meta meta;
		};

		std::vector<separate_index_meta> mg;

		for (auto it = meta.begin(), end = meta.end(); it != end; ++it) {
			if (!it->is_valid()) {
				continue;
			}

			separate_index_meta tmp;

			if (!it->error()) {
				msgpack::unpacked result;
				msgpack::unpack(&result, (const char *)it->file().data(), it->file().size());
				msgpack::object obj = result.get();

				tmp.meta = obj.as<index_meta>();
			} else if (it->error().code() == -6) {
				// do not even try to work with non-existing groups
				// next time will try to recover this group, if we reconnect
				continue;
			}

			tmp.group = it->command()->id.group_id;

			mg.emplace_back(tmp);
		}

		if (mg.empty()) {
			start_page_init();
			meta_write();
			return;
		}

		uint64_t highest_generation_number = 0;
		for (auto it = mg.begin(), end = mg.end(); it != end; ++it) {
			if (it->meta.generation_number >= highest_generation_number) {
				highest_generation_number = it->meta.generation_number;
				m_meta = it->meta;
			}
		}

		std::vector<int> recovery_groups;
		std::vector<int> good_groups;
		for (auto it = mg.begin(), end = mg.end(); it != end; ++it) {
			if (it->meta.generation_number == highest_generation_number) {
				good_groups.push_back(it->group);
			} else {
				recovery_groups.push_back(it->group);
			}
		}

		m_t.set_groups(good_groups);

		if (highest_generation_number == 0) {
			start_page_init();
			meta_write();
			return;
		}

		if (recovery_groups.empty())
			return;

		size_t pages_recovered = 0;
		for (auto it = page_begin(), end = page_end(); it != end; ++it) {
			dprintf("page: %s: %s -> %s\n", it.url().c_str(), it->str().c_str(), print_groups(recovery_groups).c_str());

			elliptics::sync_write_result wr = m_t.write(recovery_groups, it.url(), it->save(), false);
			
			recovery_groups.clear();
			for (auto r = wr.begin(), end = wr.end(); r != end; ++r) {
				if (r->is_valid() && !r->error()) {
					recovery_groups.push_back(r->command()->id.group_id);
				}
			}

			if (recovery_groups.size() == 0)
				break;

			pages_recovered++;
		}

		good_groups.insert(good_groups.end(), recovery_groups.begin(), recovery_groups.end());
		m_t.set_groups(good_groups);

		meta_write();
		printf("index: opened: page_index: %ld, groups: %s, pages recovered: %zd\n",
				m_meta.page_index, print_groups(m_t.get_groups()).c_str(), pages_recovered);
	}

	~index() {
		meta_write();
	}

	index_meta meta() const {
		return m_meta;
	}

	key search(const key &obj) const {
		auto found = search(m_sk, obj);
		if (found.second < 0)
			return key();

		return found.first.objects[found.second];
	}

	int insert(const key &obj) {
		recursion tmp;
		int ret = insert(m_sk, obj, tmp);
		if (ret < 0)
			return ret;

		m_meta.generation_number++;
		meta_write();

		return 0;
	}

	int remove(const key &obj) {
		remove_recursion tmp;
		int ret = remove(m_sk, obj, tmp);
		if (ret < 0)
			return ret;

		m_meta.generation_number++;
		meta_write();

		return 0;
	}

	iterator<T> begin(const std::string &k) const {
		key zero;
		zero.id = k;

		auto found = search(m_sk, zero);
		if (found.second < 0)
			found.second = 0;

		return iterator<T>(m_t, found.first, found.second);
	}

	iterator<T> begin() const {
		return begin(std::string("\0"));
	}

	iterator<T> end() const {
		page p;
		return iterator<T>(m_t, p, 0);
	}

	std::vector<key> keys(const std::string &start) const {
		std::vector<key> ret;
		for (auto it = begin(start), e = end(); it != e; ++it) {
			ret.push_back(*it);
		}

		return ret;
	}

	std::vector<key> keys() const {
		std::vector<key> ret;
		for (auto it = begin(), e = end(); it != e; ++it) {
			ret.push_back(*it);
		}

		return ret;
	}

	page_iterator<T> page_begin() const {
		return page_iterator<T>(m_t, m_sk);
	}

	page_iterator<T> page_end() const {
		page p;
		return page_iterator<T>(m_t, p);
	}

	std::string print_groups(const std::vector<int> &groups) const {
		std::ostringstream ss;
		for (size_t pos = 0; pos < groups.size(); ++pos) {
			ss << groups[pos];
			if (pos != groups.size() - 1)
				ss << ":";
		}

		return ss.str();
	}

private:
	T &m_t;
	eurl m_sk;

	index_meta m_meta;

	eurl meta_key() const {
		eurl ret;
		ret.bucket = m_sk.bucket;
		ret.key = m_sk.key + ".meta";
		return ret;
	}

	void meta_write() {
		std::stringstream ss;
		msgpack::pack(ss, m_meta);
		m_t.write(meta_key(), ss.str(), true);
	}

	void start_page_init() {
		page start_page;

		m_t.write(m_sk, start_page.save());
		m_meta.num_pages++;
	}

	std::pair<page, int> search(const eurl &page_key, const key &obj) const {
		elliptics::read_result_entry e = m_t.read(page_key);
		if (e.error()) {
			return std::make_pair(page(), e.error().code());
		}

		page p;
		p.load(e.file().data(), e.file().size());

		int found_pos = p.search_node(obj);
		if (found_pos < 0) {
			dprintf("search: %s: page: %s -> %s, found_pos: %d\n",
				obj.str().c_str(),
				page_key.str().c_str(), p.str().c_str(),
				found_pos);

			return std::make_pair(p, found_pos);
		}

		dprintf("search: %s: page: %s -> %s, found_pos: %d, found_key: %s\n",
			obj.str().c_str(),
			page_key.str().c_str(), p.str().c_str(),
			found_pos, p.objects[found_pos].str().c_str());

		if (p.is_leaf())
			return std::make_pair(p, found_pos);

		return search(p.objects[found_pos].url, obj);
	}

	// returns true if page at @page_key has been split after insertion
	// key used to store split part has been saved into @obj.url
	int insert(const eurl &page_key, const key &obj, recursion &rec) {
		elliptics::read_result_entry e = m_t.read(page_key);
		if (e.error()) {
			return e.error().code();
		}

		int err;
		page p;
		p.load(e.file().data(), e.file().size());

		page split;

		dprintf("insert: %s: page: %s -> %s\n", obj.str().c_str(), page_key.str().c_str(), p.str().c_str());

		if (!p.is_leaf()) {
			int found_pos = p.search_node(obj);
			if (found_pos < 0) {
				dprintf("insert: %s: page: %s -> %s, found_pos: %d\n",
					obj.str().c_str(),
					page_key.str().c_str(), p.str().c_str(),
					found_pos);

				// this is not a leaf node, but there is no leaf in @objects
				// this is the only reason non-leaf page search failed,
				// thus create new leaf node
				//
				// this path can only be taken once - when new empty index has been created
				key leaf_key;
				leaf_key.id = obj.id;
				leaf_key.url = generate_page_url();

				page leaf(true), unused_split;
				leaf.insert_and_split(obj, unused_split);
				err = check(m_t.write(leaf_key.url, leaf.save()));
				if (err)
					return err;

				// no need to perform recursion unwind, since there were no entry for this new leaf
				// which can only happen when page was originally empty
				p.insert_and_split(leaf_key, unused_split);
				p.next = leaf_key.url;
				err = check(m_t.write(page_key, p.save()));
				if (err)
					return err;

				dprintf("insert: %s: page: %s -> %s, leaf: %s -> %s\n",
						obj.str().c_str(),
						page_key.str().c_str(), p.str().c_str(),
						leaf_key.str().c_str(), leaf.str().c_str());

				m_meta.num_pages++;
				m_meta.num_leaf_pages++;
				return 0;
			}

			key &found = p.objects[found_pos];

			dprintf("insert: %s: page: %s -> %s, found_pos: %d, found_key: %s\n",
				obj.str().c_str(),
				page_key.str().c_str(), p.str().c_str(),
				found_pos, found.str().c_str());

			insert(found.url, obj, rec);

			dprintf("insert: %s: returned: %s -> %s, found_pos: %d, found_key: %s, rec: page_start: %s, split_key: %s\n",
					obj.str().c_str(),
					page_key.str().c_str(), p.str().c_str(),
					found_pos, found.str().c_str(),
					rec.page_start.str().c_str(),
					rec.split_key.str().c_str());

			// true if we should not unwind recursion and just return
			// false if either split page has to be written or page changed and has to be written
			bool want_return = true;

			if (found != rec.page_start) {
				dprintf("p: %s: replace: key: %s: id: %s -> %s\n",
						p.str().c_str(), found.str().c_str(), found.id.c_str(), rec.page_start.id.c_str());
				found.id = rec.page_start.id;

				// page has changed, it must be written into storage
				want_return = false;
			}

			if (rec.split_key) {
				p.insert_and_split(rec.split_key, split);

				// there is a split page, it was already written into the storage,
				// now its time to insert it into parent and upate parent
				want_return = false;
			}

			if (want_return) {
				rec.page_start = p.objects.front();
				rec.split_key = key();
				return 0;
			}
		} else {
			p.insert_and_split(obj, split);
		}

		rec.page_start = p.objects.front();
		rec.split_key = key();

		if (!split.is_empty()) {
			// generate key for split page
			rec.split_key.url = generate_page_url();
			rec.split_key.id = split.objects.front().id;

			split.next = p.next;
			p.next = rec.split_key.url;

			dprintf("insert: %s: write split page: %s -> %s, split: key: %s -> %s\n",
					obj.str().c_str(),
					page_key.str().c_str(), p.str().c_str(),
					rec.split_key.str().c_str(), split.str().c_str());
			err = check(m_t.write(rec.split_key.url, split.save()));
			if (err)
				return err;

			m_meta.num_pages++;
			if (p.is_leaf())
				m_meta.num_leaf_pages++;
		}

		if (!split.is_empty() && page_key == m_sk) {
			// if we split root page, put old root data into new key
			// root must always be accessible via start key
			// generate new root, which will host data for 2 new pages:
			// split and old root

			key old_root_key;
			old_root_key.url = generate_page_url();
			old_root_key.id = p.objects.front().id;

			err = check(m_t.write(old_root_key.url, p.save()));
			if (err)
				return err;

			// we have written split page and old root page above
			// now its time to create and write the new root
			page new_root, unused_split;
			new_root.insert_and_split(old_root_key, unused_split);
			new_root.insert_and_split(rec.split_key, unused_split);

			new_root.next = new_root.objects.front().url;

			err = check(m_t.write(m_sk, new_root.save()));
			if (err)
				return err;

			m_meta.num_pages++;

			dprintf("insert: %s: write split page: %s -> %s, old_root_key: %s, new_root: %s\n",
					obj.str().c_str(),
					page_key.str().c_str(), p.str().c_str(),
					old_root_key.str().c_str(), new_root.str().c_str());
		} else {
			dprintf("insert: %s: write main page: %s -> %s\n", obj.str().c_str(), page_key.str().c_str(), p.str().c_str());
			err = check(m_t.write(page_key, p.save(), true));
		}

		return err;
	}

	// returns true if page at @page_key has been split after insertion
	// key used to store split part has been saved into @obj.url
	int remove(const eurl &page_key, const key &obj, remove_recursion &rec) {
		elliptics::read_result_entry e = m_t.read(page_key);
		if (e.error()) {
			return e.error().code();
		}

		int err;
		page p;
		p.load(e.file().data(), e.file().size());

		dprintf("remove: %s: page: %s -> %s\n", obj.str().c_str(), page_key.str().c_str(), p.str().c_str());

		int found_pos = p.search_node(obj);
		if (found_pos < 0) {
			dprintf("remove: %s: page: %s -> %s, found_pos: %d\n",
				obj.str().c_str(),
				page_key.str().c_str(), p.str().c_str(),
				found_pos);

			return -ENOENT;
		}

		key &found = p.objects[found_pos];

		dprintf("remove: %s: page: %s -> %s, found_pos: %d, found_key: %s\n",
			obj.str().c_str(),
			page_key.str().c_str(), p.str().c_str(),
			found_pos, found.str().c_str());

		if (p.is_leaf() || rec.removed) {
			p.remove(found_pos);
		} else {
			err = remove(found.url, obj, rec);
			if (err < 0)
				return err;

			// we have removed key from the underlying page, and the first key of that page hasn't been changed
			if (!rec.page_start)
				return 0;

			// the first key of the underlying page has been changed, update appropriate key in the current page
			found.id = rec.page_start.id;
		}

		dprintf("remove: %s: returned: %s -> %s, found_pos: %d, found_key: %s\n",
				obj.str().c_str(),
				page_key.str().c_str(), p.str().c_str(),
				found_pos, found.str().c_str());

		rec.page_start.id.clear();
		rec.removed = false;

		if (p.objects.size() != 0) {
			// we have to update higher level page if start of the current page has been changed
			// we can not use @found here, since it could be removed from the current page
			if (found_pos == 0) {
				rec.page_start.id = p.objects.front().id;
			}

			err = check(m_t.write(page_key, p.save()));
			if (err)
				return err;
		} else {
			// if current page is empty, we have to remove appropriate link from the higher page
			rec.removed = true;

			err = check(m_t.remove(page_key));
			if (err)
				return err;

			m_meta.num_pages--;
			if (p.is_leaf())
				m_meta.num_leaf_pages--;
		}

		return 0;
	}

	eurl generate_page_url() {
		eurl ret;
		ret.bucket = m_sk.bucket;
		ret.key = m_sk.key + "." + lexical_cast(m_meta.page_index);
		dprintf("generated key: %s\n", ret.str().c_str());
		m_meta.page_index++;
		return ret;
	}

	template <typename EllipticsResult>
	int check(const EllipticsResult &wr) {
		std::vector<int> groups;
		for (auto r = wr.begin(), end = wr.end(); r != end; ++r) {
			if (!r->error()) {
				groups.push_back(r->command()->id.group_id);
			}
		}

		m_t.set_groups(groups);

		if (groups.empty())
			return -EIO;

		return 0;
	}
};

namespace intersect {
struct result {
	bool completed = false;

	// index name (eurl) -> set of keys from that index which match other indexes
	// key IDs will be the same, but key data (url) can be different
	std::map<eurl, std::vector<key>> keys;
};

template <typename T>
class intersector {
public:
	intersector(T &t) : m_t(t) {}
	result intersect(const std::vector<eurl> &indexes) const {
		std::string start = std::string("\0");
		return intersect(indexes, start, INT_MAX);
	}

	// search for intersections between all @indexes
	// starting with the key @start, returning at most @num entries
	//
	// after @intersect() completes, it sets @start to the next key to start searching from
	// user should not change that token, otherwise @intersect() may skip some entries or
	// return duplicates.
	//
	// if number of returned entries is less than requested number @num or if @start has been set to empty string
	// after call to this function returns, then intersection is completed.
	//
	// @result.completed will be set to true in this case.
	result intersect(const std::vector<eurl> &indexes, std::string &start, size_t num) const {
		struct iter {
			index<T> idx;
			indexes::iterator<T> begin, end;

			iter(T &t, const eurl &name, const std::string &start) :
				idx(t, name), begin(idx.begin(start)), end(idx.end()) {}
		};
		std::vector<iter> idata;

		for_each(indexes.begin(), indexes.end(), [&] (const eurl &name) {
				iter it(m_t, name, start);

				idata.emplace_back(it);
			});

		result res;

		while (!res.completed) {
			std::vector<int> pos;

			int current = -1;
			for (auto idata_it = idata.begin(); idata_it != idata.end(); ++idata_it) {
				auto &it = idata_it->begin;
				auto &e = idata_it->end;
				++current;

				if (it == e) {
					res.completed = true;
					break;
				}

				if (pos.size() == 0) {
					pos.push_back(current);
					continue;
				}

				auto &min_it = idata[pos[0]].begin;

				if (*it == *min_it) {
					pos.push_back(current);
					continue;
				}

				if (*it < *min_it) {
					pos.clear();
					pos.push_back(current);
				}
			}

			if (res.completed) {
				start = "";
				break;
			}

			if (pos.size() != idata.size()) {
				for (auto it = pos.begin(); it != pos.end(); ++it) {
					auto &min_it = idata[*it].begin;
					++min_it;
				}

				continue;
			}

			start = idata[pos[0]].begin->id;
			if (res.keys.begin()->second.size() == num) {
				break;
			}

			for (auto it = pos.begin(); it != pos.end(); ++it) {
				auto &min_it = idata[*it].begin;
				key k = *min_it;

				auto find_it = res.keys.find(indexes[*it]);
				if (find_it == res.keys.end()) {
					std::vector<key> kk;
					kk.emplace_back(k);
					auto pair = res.keys.insert(std::make_pair(indexes[*it], kk));
					find_it = pair.first;
				} else {
					find_it->second.emplace_back(k);
				}

				++min_it;
			}
		}

		return res;
	}
private:
	T &m_t;
};

} // ioremap::indexes::intersect

}} // ioremap::indexes

using namespace ioremap;

class elliptics_transport {
public:
	elliptics_transport(const std::string &log_file, const std::string &log_level):
	m_log(log_file.c_str(), elliptics::file_logger::parse_level(log_level)),
	m_node(elliptics::logger(m_log, blackhole::log::attributes_t())) {
	}

	void add_remotes(const std::vector<std::string> &remotes) {
		std::vector<elliptics::address> a(remotes.begin(), remotes.end());
		m_node.add_remote(a);
	}

	void set_namespace(const std::string &ns) {
		m_ns = ns;
	}

	void set_groups(const std::vector<int> &groups) {
		m_groups = groups;
	}

	std::vector<int> get_groups() const {
		return m_groups;
	}

	elliptics::read_result_entry read(const indexes::eurl &key) {
		dprintf("elliptics read: key: %s\n", key.c_str());
		elliptics::session s = session(m_groups, true);
		s.set_namespace(key.bucket);
		return s.read_data(key.key, 0, 0).get_one();
	}

	std::vector<elliptics::read_result_entry> read_all(const indexes::eurl &key) {
		std::vector<elliptics::async_read_result> results;

		elliptics::session s = session(m_groups, true);
		s.set_namespace(key.bucket);
		for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
			std::vector<int> tmp;
			tmp.push_back(*it);

			s.set_groups(tmp);

			results.emplace_back(s.read_data(key.key, 0, 0));
		}

		std::vector<elliptics::read_result_entry> ret;
		for (auto it = results.begin(), end = results.end(); it != end; ++it) {
			ret.push_back(it->get_one());
		}

		return ret;
	}

	elliptics::sync_write_result write(const std::vector<int> groups, const indexes::eurl &key, const std::string &data, bool cache) {
		dprintf("elliptics write: key: %s, data-size: %zd\n", key.c_str(), size);
		elliptics::data_pointer dp = elliptics::data_pointer::from_raw((char *)data.data(), data.size());

		elliptics::session s = session(groups, cache);
		s.set_namespace(key.bucket);

		s.set_filter(elliptics::filters::all);

		elliptics::key id(key.key);
		s.transform(id);

		dnet_io_control ctl;

		memset(&ctl, 0, sizeof(ctl));
		dnet_current_time(&ctl.io.timestamp);

		ctl.cflags = s.get_cflags();
		ctl.data = dp.data();

		ctl.io.flags = s.get_ioflags() | DNET_IO_FLAGS_PREPARE | DNET_IO_FLAGS_PLAIN_WRITE | DNET_IO_FLAGS_COMMIT;
		ctl.io.user_flags = s.get_user_flags();
		ctl.io.offset = 0;
		ctl.io.size = dp.size();
		ctl.io.num = indexes::max_page_size * 1.5;
		if (ctl.io.size > ctl.io.num) {
			ctl.io.num = ctl.io.size * 2;
		}

		memcpy(&ctl.id, &id.id(), sizeof(ctl.id));

		ctl.fd = -1;

		return s.write_data(ctl).get();
	}

	elliptics::sync_write_result write(const indexes::eurl &key, const std::string &data, bool cache = false) {
		return write(m_groups, key, data, cache);
	}

	elliptics::sync_remove_result remove(const indexes::eurl &key) {
		elliptics::session s = session(m_groups, false);
		s.set_namespace(key.bucket);
		return s.remove(key.key).get();
	}

private:
	elliptics::file_logger m_log;
	elliptics::node m_node;
	std::string m_ns;
	std::vector<int> m_groups;

	elliptics::session session(const std::vector<int> groups, bool cache) {
		elliptics::session s(m_node);
		s.set_namespace(m_ns);
		s.set_groups(groups);
		s.set_timeout(60);
		s.set_exceptions_policy(elliptics::session::no_exceptions);
		if (cache)
			s.set_ioflags(DNET_IO_FLAGS_CACHE);

		return s;
	}
};

template <typename T>
class test {
#define __stringify_1(x...)     (const char *)#x
#define __stringify(x...)       __stringify_1(x)
#define func(name, args...) __stringify(name), name, ##args
public:
	test(T &t) {
		test::run(this, func(&test::test_intersection, t, 3, 5000, 10000));
		return;

		indexes::eurl start;
		start.key = "test" + lexical_cast(rand());
		start.bucket = m_bucket;

		indexes::index<T> idx(t, start);

		test::run(this, func(&test::test_remove_some_keys, t, 10000));

		std::vector<indexes::key> keys;
		if (t.get_groups().size() > 1)
			test::run(this, func(&test::test_index_recovery, t, 10000));
		test::run(this, func(&test::test_insert_many_keys, idx, keys, 10000));
		test::run(this, func(&test::test_page_iterator, idx));
		test::run(this, func(&test::test_iterator_number, idx, keys));
		test::run(this, func(&test::test_select_many_keys, idx, keys));
		test::run(this, func(&test::test_intersection, t, 3, 5000, 10000));
	}

private:
	std::string m_bucket = "";

	template <typename Class, typename Method, typename... Args>
	static inline void run(Class *obj, const char *str, Method method, Args &&...args) {
		try {
			ribosome::timer tm;
			(obj->*method)(std::forward<Args>(args)...);
			printf("%s: %zd ms\n", str, tm.elapsed());
		} catch (const std::exception &e) {
			fprintf(stderr, "%s: failed: %s\n", str, e.what());
			exit(-1);
		}
	}

	void test_insert_many_keys(indexes::index<T> &idx, std::vector<indexes::key> &keys, int max) {
		for (int i = 0; i < max; ++i) {
			indexes::key k;

			char buf[128];

			snprintf(buf, sizeof(buf), "%08x.%08d", rand(), i);
			k.id = std::string(buf);

			snprintf(buf, sizeof(buf), "some-data.%08d", i);
			k.url.key = std::string(buf);
			k.url.bucket = m_bucket;

			dprintf("inserting: %s\n", k.str().c_str());
			idx.insert(k);
			keys.push_back(k);
			dprintf("inserted: %s\n\n", k.str().c_str());
		}
	}

	void test_remove_some_keys(T &t, int max) {
		indexes::eurl start;
		start.key = "remove-test-index." + lexical_cast(rand());
		start.bucket = m_bucket;

		indexes::index<T> idx(t, start);
		std::vector<indexes::key> keys;

		for (int i = 0; i < max; ++i) {
			indexes::key k;

			char buf[128];

			snprintf(buf, sizeof(buf), "%08x.remove-test.%08d", rand(), i);
			k.id = std::string(buf);

			snprintf(buf, sizeof(buf), "some-data.%08d", i);
			k.url.key = std::string(buf);
			k.url.bucket = m_bucket;

			idx.insert(k);
			keys.push_back(k);
		}

		ribosome::timer tm;
		printf("remove-test: meta before remove: %s\n", idx.meta().str().c_str());
		for (auto it = keys.begin(), end = keys.begin() + keys.size() / 2; it != end; ++it) {
			idx.remove(*it);
		}
		printf("remove-test: meta after remove: %s, removed entries: %zd, time: %ld ms\n",
				idx.meta().str().c_str(), keys.size() / 2, tm.elapsed());

		for (auto it = keys.begin(), end = keys.end(); it != end; ++it) {
			indexes::key found = idx.search(*it);
			if (it < keys.begin() + keys.size() / 2) {
				if (found) {
					std::ostringstream ss;
					ss << "key: " << it->str() << " has been found, but it was removed";
					throw std::runtime_error(ss.str());
				}
			} else {
				if (!found) {
					std::ostringstream ss;
					ss << "key: " << it->str() << " has not been found, but it was not removed";
					throw std::runtime_error(ss.str());
				}
			}
		}
	}

	void test_index_recovery(T &t, int max) {
		std::vector<int> groups = t.get_groups();

		indexes::eurl name;
		name.key = "recovery-test." + lexical_cast(rand());
		name.bucket = m_bucket;

		indexes::index<T> idx(t, name);

		std::vector<indexes::key> keys;

		for (int i = 0; i < max; ++i) {
			indexes::key k;
			k.id = lexical_cast(rand()) + ".recovery-key." + lexical_cast(i);
			k.url.key = "recovery-value." + lexical_cast(i);
			k.url.bucket = m_bucket;

			int err = idx.insert(k);
			if (!err) {
				keys.push_back(k);
			}

			if (i == max / 2) {
				groups = t.get_groups();
				std::vector<int> tmp;
				tmp.insert(tmp.end(), groups.begin(), groups.begin() + groups.size() / 2);
				t.set_groups(tmp);
			}
		}

		t.set_groups(groups);
		ribosome::timer tm;
		// index constructor self-heals itself
		indexes::index<T> rec(t, name);

		groups = t.get_groups();
		std::vector<int> tmp;
		tmp.insert(tmp.end(), groups.begin() + groups.size() / 2, groups.end());
		t.set_groups(tmp);

		printf("recovery: index has been self-healed, records: %d, time: %ld ms, meta: %s, reading from groups: %s\n",
				max, tm.elapsed(), rec.meta().str().c_str(), rec.print_groups(t.get_groups()).c_str());


		for (auto it = keys.begin(); it != keys.end(); ++it) {
			indexes::key s;
			s.id = it->id;

			indexes::key found = rec.search(s);
			if (!found) {
				std::ostringstream ss;
				ss << "search failed: could not find key: " << it->id.c_str();
				throw std::runtime_error(ss.str());
			}

			if (found.id != it->id) {
				std::ostringstream ss;
				ss << "search failed: ID mismatch: found: " << found << ", must be: " << *it;
				throw std::runtime_error(ss.str());
			}

			if (found.url != it->url) {
				std::ostringstream ss;
				ss << "search failed: url/value mismatch: found: " << found << ", must be: " << *it;
				throw std::runtime_error(ss.str());
			}

			dprintf("search: key: %s, url/value: %s\n", found.id.c_str(), found.url.str().c_str());
		}

		t.set_groups(groups);
	}

	void test_select_many_keys(indexes::index<T> &idx, std::vector<indexes::key> &keys) {
		for (auto it = keys.begin(); it != keys.end(); ++it) {
			indexes::key k;

			k.id = it->id;
			indexes::key found = idx.search(k);
			if (!found) {
				std::ostringstream ss;
				ss << "search failed: could not find key: " << it->id.c_str();
				throw std::runtime_error(ss.str());
			}

			if (found.id != it->id) {
				std::ostringstream ss;
				ss << "search failed: ID mismatch: found: " << found << ", must be: " << *it;
				throw std::runtime_error(ss.str());
			}
			if (found.url != it->url) {
				std::ostringstream ss;
				ss << "search failed: url/value mismatch: found: " << found << ", must be: " << *it;
				throw std::runtime_error(ss.str());
			}

			dprintf("search: key: %s, url/value: %s\n\n", found.id.c_str(), found.value.str().c_str());
		}
	}

	void test_iterator_number(indexes::index<T> &idx, std::vector<indexes::key> &keys) {
		size_t num = 0;
		for (auto it = idx.begin(), end = idx.end(); it != end; ++it) {
			dprintf("iterator: %s\n", it->str().c_str());
			num++;
		}

		if (num != keys.size()) {
			std::ostringstream ss;
			ss << "iterated number mismatch: keys: " << keys.size() << ", iterated: " << num;
			throw std::runtime_error(ss.str());
		}
	}

	void test_page_iterator(indexes::index<T> &idx) {
		size_t page_num = 0;
		size_t leaf_num = 0;
		for (auto it = idx.page_begin(), end = idx.page_end(); it != end; ++it) {
			dprintf("page: %s: %s\n", it.path().c_str(), it->str().c_str());
			page_num++;
			if (it->is_leaf())
				leaf_num++;
		}
		indexes::index_meta meta = idx.meta();
		printf("meta: %s\n", meta.str().c_str());

		if (page_num != meta.num_pages) {
			std::ostringstream ss;
			ss << "page iterator: number of pages mismatch: "
				"meta: " << meta.str() <<
				"iterated: number of pages: " << page_num <<
				", number of leaf pages: " << leaf_num <<
				std::endl;
			throw std::runtime_error(ss.str());
		}

		if (leaf_num != meta.num_leaf_pages) {
			std::ostringstream ss;
			ss << "page iterator: number of leaf pages mismatch: "
				"meta: " << meta.str() <<
				"iterated: number of pages: " << page_num <<
				", number of leaf pages: " << leaf_num <<
				std::endl;
			throw std::runtime_error(ss.str());
		}
	}

	void test_intersection(T &t, int num_indexes, size_t same_num, size_t different_num) {
		std::vector<indexes::eurl> indexes;
		std::vector<indexes::key> same;

		for (size_t i = 0; i < same_num; ++i) {
			indexes::key k;
			k.id = lexical_cast(rand()) + ".url-same-key." + lexical_cast(i);
			k.url.key = "url-same-data." + lexical_cast(i);
			k.url.bucket = m_bucket;

			same.emplace_back(k);
		}

		for (int i = 0; i < num_indexes; ++i) {
			indexes::eurl url;
			url.bucket = m_bucket;
			url.key = "intersection-index.rand." + lexical_cast(i) + "." + lexical_cast(rand());
			indexes.push_back(url);

			indexes::index<T> idx(t, url);

			for (size_t j = 0; j < different_num; ++j) {
				indexes::key k;

				k.id = lexical_cast(rand()) + ".url-random-key." + lexical_cast(i);
				k.url.key = "url-random-data." + lexical_cast(i);
				k.url.bucket = m_bucket;

				idx.insert(k);
			}

			for (auto it = same.begin(); it != same.end(); ++it) {
				idx.insert(*it);
			}
		}

		struct index_checker {
			index_checker(const indexes::intersect::result &res, size_t same_num) {
				size_t size = 0;
				for (auto it = res.keys.begin(), end = res.keys.end(); it != end; ++it) {
					if (!size) {
						size = it->second.size();
						continue;
					}

					if (size != it->second.size() || size != same_num) {
						std::ostringstream ss;
						ss << "intersection failed: indexes: " << res.keys.size() <<
							", same keys in each index: " << same_num <<
							", current-index: " << it->first.str() <<
							", found keys (must be equal to the same jeys in each index): " << it->second.size();
						throw std::runtime_error(ss.str());
					}
				}

				for (size_t i = 0; i < size; ++i) {
					indexes::key k;
					for (auto it = res.keys.begin(), end = res.keys.end(); it != end; ++it) {
						if (!k) {
							k = it->second[i];
							continue;
						}

						if (k != it->second[i]) {
							std::ostringstream ss;
							ss << "intersection failed: indexes: " << res.keys.size() <<
								", same keys in each index: " << same_num <<
								", current-index: " << it->first.str() <<
								", mismatch position: " << i <<
								", found key: " << it->second[i].str() <<
								", must be: " << k.str();
							throw std::runtime_error(ss.str());
						}
					}
				}
			}
		};

		ribosome::timer tm;
		indexes::intersect::intersector<T> inter(t);
		indexes::intersect::result res = inter.intersect(indexes);
		for (auto it = res.keys.begin(); it != res.keys.end(); ++it) {
			printf("index: %s, keys: %zd\n", it->first.str().c_str(), it->second.size());
			for (auto k = it->second.begin(); k != it->second.end(); ++k) {
				dprintf("  %s\n", k->str().c_str());
			}
		}

		printf("intersection: indexes: %d, found keys: %zd, must be: %zd, total keys in each index: %zd, time: %ld ms\n",
				num_indexes, res.keys.size(), same_num, same_num + different_num, tm.restart());

		index_checker c(res, same_num);

		indexes::intersect::intersector<T> p(t);
		std::string start("\0");
		size_t num = 100;
		size_t num_found = 0;

		while (true) {
			indexes::intersect::result res = p.intersect(indexes, start, num);

			if (!res.keys.size())
				break;

			size_t cur_size = res.keys.begin()->second.size();
			num_found += cur_size;

			for (auto it = res.keys.begin(); it != res.keys.end(); ++it) {
				printf("index: %s, keys: %zd, total keys found: %zd\n", it->first.str().c_str(), cur_size, num_found);
				for (auto k = it->second.begin(); k != it->second.end(); ++k) {
					dprintf("  %s\n", k->str().c_str());
				}
			}

			index_checker c(res, cur_size);

			if (cur_size < num)
				break;

			if (res.completed)
				break;
		}

		printf("paginated intersection: indexes: %d, found keys: %zd, must be: %zd, total keys in each index: %zd, time: %ld ms\n",
				num_indexes, num_found, same_num, same_num + different_num, tm.restart());
		if (num_found != same_num) {
			std::ostringstream ss;
			ss << "paginated intersection failed: indexes: " << num_indexes << ", same keys in each index: " << same_num <<
				", found keys: " << num_found <<
				", total keys in each index: " << different_num + same_num;
			throw std::runtime_error(ss.str());
		}
	}
};

int main(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	std::vector<std::string> remotes;


	bpo::options_description generic("Index test options");
	generic.add_options()
		("help", "This help message")
		;

	std::string ns, log_file, log_level, groups;
	bpo::options_description ell("Elliptics options");
	ell.add_options()
		("remote", bpo::value<std::vector<std::string>>(&remotes)->required()->composing(), "remote node: addr:port:family")
		("log-file", bpo::value<std::string>(&log_file)->default_value("/dev/stdout"), "log file")
		("log-level", bpo::value<std::string>(&log_level)->default_value("error"), "log level: error, info, notice, debug")
		("groups", bpo::value<std::string>(&groups)->required(), "groups where index tree is stored: 1:2:3")
		("namespace", bpo::value<std::string>(&ns)->default_value(""), "Namespace where index tree is stored")
		;

	bpo::options_description cmdline_options;
	cmdline_options.add(generic).add(ell);

	bpo::variables_map vm;

	try {
		bpo::store(bpo::command_line_parser(argc, argv).options(cmdline_options).run(), vm);

		if (vm.count("help")) {
			std::cout << cmdline_options << std::endl;
			return 0;
		}

		bpo::notify(vm);
	} catch (const std::exception &e) {
		std::cerr << "Invalid options: " << e.what() << "\n" << cmdline_options << std::endl;
		return -1;
	}

	elliptics_transport t(log_file, log_level);
	t.add_remotes(remotes);
	t.set_namespace(ns);
	t.set_groups(elliptics::parse_groups(groups.c_str()));

	time_t tm = time(NULL);
	srand(tm);

	dprintf("index: init: t: %zd\n", tm);

	test<elliptics_transport> tt(t);
}
