#ifndef __INDEXES_ELLIPTICS_HPP
#define __INDEXES_ELLIPTICS_HPP

#include "greylock/error.hpp"
#include "greylock/core.hpp"

#include <elliptics/session.hpp>

namespace ioremap { namespace greylock {

class elliptics_transport {
public:
	elliptics_transport(const std::string &log_file, const std::string &log_level):
	m_log(log_file.c_str(), elliptics::file_logger::parse_level(log_level)),
	m_node(new elliptics::node(elliptics::logger(m_log, blackhole::log::attributes_t()))) {
	}

	std::shared_ptr<elliptics::node> get_node() {
		return m_node;
	}

	const elliptics::logger &logger() const {
		return m_node->get_log();
	}

	void add_remotes(const std::vector<std::string> &remotes) {
		std::vector<elliptics::address> a(remotes.begin(), remotes.end());
		m_node->add_remote(a);
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

	status get_bucket(size_t size) const {
		(void) size;

		status st;
		st.data = elliptics::data_pointer::copy(m_ns);
		return st;
	}

	status read(const greylock::eurl &key) {
		elliptics::session s = session(m_groups, true);
		s.set_namespace(key.bucket);
		return status(s.read_data(key.key, 0, 0).get_one());
	}

	std::vector<status> read_all(const greylock::eurl &key) {
		std::vector<elliptics::async_read_result> results;

		elliptics::session s = session(m_groups, true);
		s.set_namespace(key.bucket);
		for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
			std::vector<int> tmp;
			tmp.push_back(*it);

			s.set_groups(tmp);

			results.emplace_back(s.read_data(key.key, 0, 0));
		}

		std::vector<status> ret;
		for (auto it = results.begin(), end = results.end(); it != end; ++it) {
			ret.push_back(status(it->get_one()));
		}

		return ret;
	}

	std::vector<status> write(const std::vector<int> groups, const greylock::eurl &key,
			const std::string &data, size_t reserve_size, bool cache) {
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
		ctl.io.num = reserve_size;
		if (ctl.io.size > ctl.io.num) {
			ctl.io.num = ctl.io.size * 1.5;
		}

		memcpy(&ctl.id, &id.id(), sizeof(ctl.id));

		ctl.fd = -1;

		BH_LOG(logger(), DNET_LOG_NOTICE, "%s: elliptics write: key: %s, data-size: %d, reserve-size: %d, cache: %d\n",
				dnet_dump_id(&id.id()),
				key.str().c_str(), data.size(), reserve_size, cache);

		std::vector<status> ret;
		elliptics::sync_write_result res = s.write_data(ctl).get();
		for (auto it = res.begin(), end = res.end(); it != end; ++it) {
			ret.emplace_back(status(*it));
		}

		return ret;
	}

	std::vector<status> write(const greylock::eurl &key, const std::string &data, bool cache = false) {
		return write(m_groups, key, data, default_reserve_size, cache);
	}

	std::vector<status> remove(const greylock::eurl &key) {
		elliptics::session s = session(m_groups, false);
		s.set_namespace(key.bucket);

		elliptics::sync_remove_result res = s.remove(key.key).get();
		std::vector<status> ret;
		for (auto it = res.begin(), end = res.end(); it != end; ++it) {
			ret.emplace_back(status(*it));
		}

		return ret;
	}

	std::string generate(const std::string &ns, const std::string &key) const {
		elliptics::session s(*m_node);
		s.set_namespace(ns.data(), ns.size());
		elliptics::key k(key);
		s.transform(k);

		DNET_DUMP_ID_LEN(name, &k.id(), DNET_ID_SIZE);
		return std::string(name);
	}


private:
	elliptics::file_logger m_log;
	std::shared_ptr<elliptics::node> m_node;
	std::string m_ns;
	std::vector<int> m_groups;

	elliptics::session session(const std::vector<int> groups, bool cache) {
		elliptics::session s(*m_node);
		s.set_namespace(m_ns);
		s.set_groups(groups);
		s.set_timeout(60);
		s.set_exceptions_policy(elliptics::session::no_exceptions);
		if (cache)
			s.set_ioflags(DNET_IO_FLAGS_CACHE);

		return s;
	}
};

}} // namespace ioremap

#endif // __INDEXES_ELLIPTICS_HPP
