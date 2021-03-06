#ifndef __INDEXES_KEY_HPP
#define __INDEXES_KEY_HPP

#include "greylock/core.hpp"

#include <string>

namespace ioremap { namespace greylock {

struct key {
	std::string id;
	eurl url;
	uint64_t timestamp = 0;
	std::vector<size_t> positions;

	MSGPACK_DEFINE(id, url, positions, timestamp);

	void set_timestamp(long tsec, long nsec) {
		timestamp = tsec;
		timestamp <<= 30;
		timestamp |= nsec & ((1<<30) - 1);
	}

	void get_timestamp(long &tsec, long &nsec) const {
		tsec = timestamp >> 30;
		nsec = timestamp & ((1<<30) - 1);
	}

	size_t size() const {
		return id.size() + url.size();
	}

	bool operator<(const key &other) const {
		if (timestamp < other.timestamp)
			return true;
		if (timestamp > other.timestamp)
			return false;
		return id < other.id;
	}
	bool operator<=(const key &other) const {
		if (timestamp < other.timestamp)
			return true;
		if (timestamp > other.timestamp)
			return false;
		return id <= other.id;
	}
	bool operator==(const key &other) const {
		return (timestamp == other.timestamp) && (id == other.id);
	}
	bool operator!=(const key &other) const {
		return id != other.id || (timestamp != other.timestamp);
	}

	operator bool() const {
		return id.size() != 0;
	}
	bool operator !() const {
		return !operator bool();
	}

	std::string str() const {
		long tsec, tnsec;
		get_timestamp(tsec, tnsec);
		return id + ":" + url.str() + ":" + elliptics::lexical_cast(tsec) + "." + elliptics::lexical_cast(tnsec);
	}
};


}} // namespace ioremap::greylock

#endif // __INDEXES_KEY_HPP
