/*
 * 2012+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <iostream>
#include <deque>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>

#include <boost/unordered_map.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

#include "../library/elliptics.h"
#include "../indexes/local_session.h"

#include "elliptics/packet.h"
#include "elliptics/interface.h"

namespace ioremap { namespace cache {

class raw_data_t {
	public:
		raw_data_t(const char *data, size_t size) {
			m_data.reserve(size);
			m_data.insert(m_data.begin(), data, data + size);
		}

		std::vector<char> &data(void) {
			return m_data;
		}

		size_t size(void) {
			return m_data.size();
		}

	private:
		std::vector<char> m_data;
};

struct data_lru_tag_t;
typedef boost::intrusive::list_base_hook<boost::intrusive::tag<data_lru_tag_t>,
					 boost::intrusive::link_mode<boost::intrusive::safe_link>
					> lru_list_base_hook_t;
struct data_set_tag_t;
typedef boost::intrusive::set_base_hook<boost::intrusive::tag<data_set_tag_t>,
					 boost::intrusive::link_mode<boost::intrusive::safe_link>
					> set_base_hook_t;

struct time_set_tag_t;
typedef boost::intrusive::set_base_hook<boost::intrusive::tag<time_set_tag_t>,
					 boost::intrusive::link_mode<boost::intrusive::safe_link>
					> time_set_base_hook_t;

struct sync_set_tag_t;
typedef boost::intrusive::set_base_hook<boost::intrusive::tag<sync_set_tag_t>,
					 boost::intrusive::link_mode<boost::intrusive::safe_link>
					> sync_set_base_hook_t;

class data_t : public lru_list_base_hook_t, public set_base_hook_t, public time_set_base_hook_t, public sync_set_base_hook_t {
	public:
		data_t(const unsigned char *id) {
			memcpy(m_id.id, id, DNET_ID_SIZE);
		}

		data_t(const unsigned char *id, size_t lifetime, const char *data, size_t size, bool remove_from_disk) :
			m_lifetime(0), m_synctime(0), m_user_flags(0), m_remove_from_disk(remove_from_disk) {
			memcpy(m_id.id, id, DNET_ID_SIZE);
			dnet_empty_time(&m_timestamp);

			if (lifetime)
				m_lifetime = lifetime + time(NULL);

			m_data.reset(new raw_data_t(data, size));
		}

		data_t(const data_t &other) = delete;
		data_t &operator =(const data_t &other) = delete;

		~data_t() {
		}

		const struct dnet_raw_id &id(void) const {
			return m_id;
		}

		std::shared_ptr<raw_data_t> data(void) const {
			return m_data;
		}

		size_t lifetime(void) const {
			return m_lifetime;
		}

		void set_lifetime(size_t lifetime) {
			m_lifetime = lifetime;
		}

		size_t synctime() const {
			return m_synctime;
		}

		void set_synctime(size_t synctime) {
			m_synctime = synctime;
		}

		void clear_synctime() {
			m_synctime = 0;
		}

		const dnet_time &timestamp() const {
			return m_timestamp;
		}

		void set_timestamp(const dnet_time &timestamp) {
			m_timestamp = timestamp;
		}

		uint64_t user_flags() const {
			return m_user_flags;
		}

		void set_user_flags(uint64_t user_flags) {
			m_user_flags = user_flags;
		}

		bool remove_from_disk() const {
			return m_remove_from_disk;
		}

		size_t size(void) const {
			return m_data->size();
		}

		friend bool operator< (const data_t &a, const data_t &b) {
			return dnet_id_cmp_str(a.id().id, b.id().id) < 0;
		}

		friend bool operator> (const data_t &a, const data_t &b) {
			return dnet_id_cmp_str(a.id().id, b.id().id) > 0;
		}

		friend bool operator== (const data_t &a, const data_t &b) {
			return dnet_id_cmp_str(a.id().id, b.id().id) == 0;
		}

	private:
		size_t m_lifetime;
		size_t m_synctime;
		dnet_time m_timestamp;
		uint64_t m_user_flags;
		bool m_remove_from_disk;
		struct dnet_raw_id m_id;
		std::shared_ptr<raw_data_t> m_data;
};

typedef boost::intrusive::list<data_t, boost::intrusive::base_hook<lru_list_base_hook_t> > lru_list_t;
typedef boost::intrusive::set<data_t, boost::intrusive::base_hook<set_base_hook_t>,
					  boost::intrusive::compare<std::less<data_t> >
			     > iset_t;

struct lifetime_less {
	bool operator() (const data_t &x, const data_t &y) const {
		return x.lifetime() < y.lifetime()
			|| (x.lifetime() == y.lifetime() && ((&x) < (&y)));
	}
};

typedef boost::intrusive::set<data_t, boost::intrusive::base_hook<time_set_base_hook_t>,
					  boost::intrusive::compare<lifetime_less>
			     > life_set_t;

struct synctime_less {
	bool operator() (const data_t &x, const data_t &y) const {
		return x.synctime() < y.synctime()
			|| (x.synctime() == y.synctime() && ((&x) < (&y)));
	}
};

typedef boost::intrusive::set<data_t, boost::intrusive::base_hook<sync_set_base_hook_t>,
					  boost::intrusive::compare<synctime_less>
			     > sync_set_t;

class cache_t {
	public:
		cache_t(struct dnet_node *n, size_t max_size) :
		m_need_exit(false),
		m_node(n),
		m_cache_size(0),
		m_max_cache_size(max_size) {
			m_lifecheck = std::thread(std::bind(&cache_t::life_check, this));
		}

		~cache_t() {
			stop();
			m_lifecheck.join();

			m_max_cache_size = 0;
			resize(0);
		}

		void stop(void) {
			m_need_exit = true;
		}

		int write(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io, const char *data) {
			const size_t lifetime = io->start;
			const size_t size = io->size;
			const bool remove_from_disk = (io->flags & DNET_IO_FLAGS_CACHE_REMOVE_FROM_DISK);
			const bool cache = (io->flags & DNET_IO_FLAGS_CACHE);
			const bool cache_only = (io->flags & DNET_IO_FLAGS_CACHE_ONLY);
			const bool append = (io->flags & DNET_IO_FLAGS_APPEND);

			std::lock_guard<std::mutex> guard(m_lock);

			iset_t::iterator it = m_set.find(id);

			if (it == m_set.end()) {
				// If file not found and CACHE flag is not set - fallback to backend request
				if (!cache)
					return -ENOTSUP;

				if (!cache_only) {
					int err = 0;
					it = populate_from_disk(id, remove_from_disk, &err);

					if (err != 0 && err != -ENOENT)
						return err;
				}

				// Create empty data for code simplifing
				if (it == m_set.end())
					it = create_data(id, 0, 0, remove_from_disk);
			}

			raw_data_t &raw = *it->data();

			if (io->flags & DNET_IO_FLAGS_COMPARE_AND_SWAP) {
				// Data is already in memory, so it's free to use it
				// raw.size() is zero only if there is no such file on the server
				if (raw.size() != 0) {
					struct dnet_raw_id csum;
					dnet_transform_node(m_node, raw.data().data(), raw.size(), csum.id, sizeof(csum.id));

					if (memcmp(csum.id, io->parent, DNET_ID_SIZE)) {
						dnet_log(m_node, DNET_LOG_ERROR, "%s: cas: cache checksum mismatch\n", dnet_dump_id(&cmd->id));
						return -EBADFD;
					}
				}
			}

			size_t new_size = 0;

			if (append) {
				new_size = raw.size() + size;
			} else {
				new_size = io->offset + io->size;
			}

			// Recalc used space, free enough space for new data, move object to the end of the queue
			m_cache_size -= raw.size();
			m_lru.erase(m_lru.iterator_to(*it));

			if (m_cache_size + new_size > m_max_cache_size)
				resize(new_size * 2);

			m_lru.push_back(*it);
			m_cache_size += new_size;

			if (append) {
				raw.data().insert(raw.data().end(), data, data + size);
			} else {
				raw.data().resize(new_size);
				memcpy(raw.data().data() + io->offset, data, size);
			}

			// Mark data as dirty one, so it will be synced to the disk
			if (!it->synctime() && !(io->flags & DNET_IO_FLAGS_CACHE_ONLY)) {
				it->set_synctime(time(NULL) + m_node->cache_sync_timeout);
				m_syncset.insert(*it);
			}

			if (it->lifetime())
				m_lifeset.erase(m_lifeset.iterator_to(*it));

			if (lifetime) {
				it->set_lifetime(lifetime + time(NULL));
				m_lifeset.insert(*it);
			}

			it->set_timestamp(io->timestamp);
			it->set_user_flags(io->user_flags);

			return 0;
		}

		std::shared_ptr<raw_data_t> read(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io) {
			const bool cache = (io->flags & DNET_IO_FLAGS_CACHE);
			const bool cache_only = (io->flags & DNET_IO_FLAGS_CACHE_ONLY);
			(void) cmd;

			std::lock_guard<std::mutex> guard(m_lock);

			iset_t::iterator it = m_set.find(id);
			if (it == m_set.end() && cache && !cache_only) {
				int err = 0;
				it = populate_from_disk(id, false, &err);
			}

			if (it != m_set.end()) {
				m_lru.erase(m_lru.iterator_to(*it));
				m_lru.push_back(*it);

				io->timestamp = it->timestamp();
				io->user_flags = it->user_flags();
				return it->data();
			}

			return std::shared_ptr<raw_data_t>();
		}

		int remove(const unsigned char *id, dnet_io_attr *io) {
			const bool cache_only = (io->flags & DNET_IO_FLAGS_CACHE_ONLY);
			bool remove_from_disk = false;
			int err = -ENOENT;

			std::unique_lock<std::mutex> guard(m_lock);
			iset_t::iterator it = m_set.find(id);
			if (it != m_set.end()) {
				// If cache_only is not set the data also should be remove from the disk
				// If data is marked and cache_only is not set - data must be synced to the disk
				remove_from_disk = (it->remove_from_disk() || !cache_only);
				if (it->synctime() && !cache_only) {
					m_syncset.erase(m_syncset.iterator_to(*it));
					it->clear_synctime();
				}
				erase_element(&(*it));
				err = 0;
			}

			guard.unlock();

			if (remove_from_disk) {
				struct dnet_id raw;
				memset(&raw, 0, sizeof(struct dnet_id));

				dnet_setup_id(&raw, 0, (unsigned char *)id);

				int local_err = dnet_remove_local(m_node, &raw);
				if (local_err != -ENOENT)
					err = local_err;
			}

			return err;
		}

	private:
		bool m_need_exit;
		struct dnet_node *m_node;
		size_t m_cache_size, m_max_cache_size;
		std::mutex m_lock;
		iset_t m_set;
		lru_list_t m_lru;
		life_set_t m_lifeset;
		sync_set_t m_syncset;
		std::thread m_lifecheck;

		cache_t(const cache_t &) = delete;

		iset_t::iterator create_data(const unsigned char *id, const char *data, size_t size, bool remove_from_disk) {
			if (m_cache_size + size > m_max_cache_size)
				resize(size);

			data_t *raw = new data_t(id, 0, data, size, remove_from_disk);

			m_cache_size += size;

			m_lru.push_back(*raw);
			return m_set.insert(*raw).first;
		}

		iset_t::iterator populate_from_disk(const unsigned char *id, bool remove_from_disk, int *err) {
			local_session sess(m_node);
			sess.set_ioflags(DNET_IO_FLAGS_NOCACHE);

			dnet_id raw_id;
			memset(&raw_id, 0, sizeof(raw_id));
			memcpy(raw_id.id, id, DNET_ID_SIZE);

			uint64_t user_flags = 0;
			dnet_time timestamp;
			dnet_empty_time(&timestamp);

			ioremap::elliptics::data_pointer data = sess.read(raw_id, &user_flags, &timestamp, err);

			if (*err == 0) {
				auto it = create_data(id, reinterpret_cast<char *>(data.data()), data.size(), remove_from_disk);
				it->set_user_flags(user_flags);
				it->set_timestamp(timestamp);
				return it;
			}

			return m_set.end();
		}

		void resize(size_t reserve) {
			while (!m_lru.empty()) {
				data_t *raw = &m_lru.front();

				erase_element(raw);


				/* break early if free space in cache more than requested reserve */
				if (m_max_cache_size > reserve + m_cache_size)
					break;
			}
		}

		void erase_element(data_t *obj) {
			m_lru.erase(m_lru.iterator_to(*obj));
			m_set.erase(m_set.iterator_to(*obj));
			if (obj->lifetime())
				m_lifeset.erase(m_lifeset.iterator_to(*obj));

			if (obj->synctime()) {
				sync_element(obj);
			}

			m_cache_size -= obj->size();

			delete obj;
		}

		void sync_element(data_t *obj) {
			local_session sess(m_node);
			sess.set_ioflags(DNET_IO_FLAGS_NOCACHE);

			struct dnet_id raw;
			memset(&raw, 0, sizeof(struct dnet_id));
			memcpy(raw.id, obj->id().id, DNET_ID_SIZE);

			auto &data = obj->data()->data();
			int err = sess.write(raw, data.data(), data.size());
			if (err) {
				dnet_log(m_node, DNET_LOG_ERROR, "%s: forced to sync to disk, err: %d\n", dnet_dump_id_str(raw.id), err);
			}

			m_syncset.erase(m_syncset.iterator_to(*obj));
			obj->clear_synctime();
		}

		void life_check(void) {
			local_session sess(m_node);
			sess.set_ioflags(DNET_IO_FLAGS_NOCACHE);

			while (!m_need_exit) {
				std::deque<struct dnet_id> remove;

				while (!m_need_exit && !m_lifeset.empty()) {
					size_t time = ::time(NULL);

					std::lock_guard<std::mutex> guard(m_lock);

					if (m_lifeset.empty())
						break;

					life_set_t::iterator it = m_lifeset.begin();
					if (it->lifetime() > time)
						break;

					if (it->remove_from_disk()) {
						struct dnet_id id;
						memset(&id, 0, sizeof(struct dnet_id));

						dnet_setup_id(&id, 0, (unsigned char *)it->id().id);

						remove.push_back(id);
					}

					erase_element(&(*it));
				}

				while (!m_need_exit && !m_syncset.empty()) {
					size_t time = ::time(NULL);

					std::lock_guard<std::mutex> guard(m_lock);

					if (m_syncset.empty())
						break;

					sync_set_t::iterator it = m_syncset.begin();
					if (it->synctime() > time)
						break;

					sync_element(&*it);

				}

				for (std::deque<struct dnet_id>::iterator it = remove.begin(); it != remove.end(); ++it) {
					dnet_remove_local(m_node, &(*it));
				}

				sleep(1);
			}
		}
};

class cache_manager {
	public:
		cache_manager(struct dnet_node *n, int num = 16) {
			for (int i  = 0; i < num; ++i) {
				m_caches.emplace_back(std::make_shared<cache_t>(n, n->cache_size / num));
			}
		}

		~cache_manager() {
			for (auto it = m_caches.begin(); it != m_caches.end(); ++it) {
				(*it)->stop();
			}
		}

		int write(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io, const char *data) {
			return m_caches[idx(id)]->write(id, cmd, io, data);
		}

		std::shared_ptr<raw_data_t> read(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io) {
			return m_caches[idx(id)]->read(id, cmd, io);
		}

		int remove(const unsigned char *id, dnet_io_attr *io) {
			return m_caches[idx(id)]->remove(id, io);
		}

	private:
		std::vector<std::shared_ptr<cache_t>> m_caches;

		int idx(const unsigned char *id) {
			int i = *(int *)id;
			return i % m_caches.size();
		}
};

}}

using namespace ioremap::cache;

int dnet_cmd_cache_io(struct dnet_net_state *st, struct dnet_cmd *cmd, struct dnet_io_attr *io, char *data)
{
	struct dnet_node *n = st->n;
	int err = -ENOTSUP;

	if (!n->cache) {
		dnet_log(n, DNET_LOG_ERROR, "%s: cache is not supported\n", dnet_dump_id(&cmd->id));
		return -ENOTSUP;
	}

	cache_manager *cache = (cache_manager *)n->cache;
	std::shared_ptr<raw_data_t> d;

	try {
		switch (cmd->cmd) {
			case DNET_CMD_WRITE:
				err = cache->write(io->id, cmd, io, data);
				break;
			case DNET_CMD_READ:
				d = cache->read(io->id, cmd, io);
				if (!d) {
					if (!(io->flags & DNET_IO_FLAGS_CACHE)) {
						return -ENOTSUP;
					}

					err = -ENOENT;
					break;
				}

				if (io->offset + io->size > d->size()) {
					dnet_log_raw(n, DNET_LOG_ERROR, "%s: %s cache: invalid offset/size: "
							"offset: %llu, size: %llu, cached-size: %zd\n",
							dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd),
							(unsigned long long)io->offset, (unsigned long long)io->size,
							d->size());
					err = -EINVAL;
					break;
				}

				if (io->size == 0)
					io->size = d->size() - io->offset;

				cmd->flags &= ~DNET_FLAGS_NEED_ACK;
				err = dnet_send_read_data(st, cmd, io, (char *)d->data().data() + io->offset, -1, io->offset, 0);
				break;
			case DNET_CMD_DEL:
				err = cache->remove(cmd->id.id, io);
				break;
		}
	} catch (const std::exception &e) {
		dnet_log_raw(n, DNET_LOG_ERROR, "%s: %s cache operation failed: %s\n",
				dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), e.what());
		err = -ENOENT;
	}

	if ((cmd->cmd == DNET_CMD_WRITE) && !err) {
		cmd->flags &= ~DNET_FLAGS_NEED_ACK;
		err = dnet_send_file_info_without_fd(st, cmd, 0, io->size);
	}

	return err;
}

int dnet_cache_init(struct dnet_node *n)
{
	if (!n->cache_size)
		return 0;

	try {
		n->cache = (void *)(new cache_manager(n, 16));
	} catch (const std::exception &e) {
		dnet_log_raw(n, DNET_LOG_ERROR, "Could not create cache: %s\n", e.what());
		return -ENOMEM;
	}

	return 0;
}

void dnet_cache_cleanup(struct dnet_node *n)
{
	if (n->cache)
		delete (cache_manager *)n->cache;
}
