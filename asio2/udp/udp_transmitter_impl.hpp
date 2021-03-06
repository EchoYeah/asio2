/*
 * COPYRIGHT (C) 2017, zhllxt
 *
 * author   : zhllxt
 * qq       : 37792738
 * email    : 37792738@qq.com
 * 
 */

#ifndef __ASIO2_UDP_TRANSMITTER_IMPL_HPP__
#define __ASIO2_UDP_TRANSMITTER_IMPL_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <asio2/base/transmitter_impl.hpp>

namespace asio2
{

	class udp_transmitter_impl : public transmitter_impl
	{
	public:
		/**
		 * @construct
		 */
		explicit udp_transmitter_impl(
			std::shared_ptr<url_parser>       url_parser_ptr,
			std::shared_ptr<listener_mgr>     listener_mgr_ptr,
			std::shared_ptr<asio::io_context> send_io_context_ptr,
			std::shared_ptr<asio::io_context> recv_io_context_ptr
		)
			: transmitter_impl(url_parser_ptr, listener_mgr_ptr, send_io_context_ptr, recv_io_context_ptr)
			, m_socket(*m_recv_io_context_ptr)
		{
		}

		/**
		 * @destruct
		 */
		virtual ~udp_transmitter_impl()
		{
		}

		/**
		 * @function : start the sender.you must call the stop function before application exit,otherwise will cause crash.
		 * @return   : true  - start successed 
		 *             false - start failed
		 */
		virtual bool start() override
		{
			try
			{
				m_state = state::starting;

				// reset the state to the default
				m_fire_close_is_called.clear(std::memory_order_release);

				// parse address and port
				asio::ip::udp::resolver resolver(*m_recv_io_context_ptr);
				asio::ip::udp::resolver::query query(m_url_parser_ptr->get_ip(), m_url_parser_ptr->get_port());
				asio::ip::udp::endpoint endpoint = *resolver.resolve(query);

				m_socket.open(endpoint.protocol());

				// setsockopt SO_SNDBUF from url params
				if (m_url_parser_ptr->get_so_sndbuf_size() > 0)
				{
					asio::socket_base::send_buffer_size option(m_url_parser_ptr->get_so_sndbuf_size());
					m_socket.set_option(option);
				}

				// setsockopt SO_RCVBUF from url params
				if (m_url_parser_ptr->get_so_rcvbuf_size() > 0)
				{
					asio::socket_base::receive_buffer_size option(m_url_parser_ptr->get_so_rcvbuf_size());
					m_socket.set_option(option);
				}

				// when you close socket in linux system,and start socket immediate,you will get like this "the address is in use",
				// and bind is failed,but i'm suer i close the socket correct already before,why does this happen? the reasion is 
				// the socket option "TIME_WAIT",although you close the socket,but the system not release the socket,util 2~4 
				// seconds later,so we can use the SO_REUSEADDR option to avoid this problem,like below
				m_socket.set_option(asio::ip::tcp::acceptor::reuse_address(true)); // set port reuse
				m_socket.bind(endpoint);

				m_state = state::started;

				_post_recv(shared_from_this(), std::make_shared<buffer<uint8_t>>(
					m_url_parser_ptr->get_recv_buffer_size(), malloc_recv_buffer(m_url_parser_ptr->get_recv_buffer_size()), 0));

				m_state = state::running;

				return (m_socket.is_open());
			}
			catch (asio::system_error & e)
			{
				set_last_error(e.code().value());
			}

			return false;
		}

		/**
		 * @function : stop the sender
		 */
		virtual void stop() override
		{
			if (m_state >= state::starting)
			{
				auto prev_state = m_state;
				m_state = state::stopping;

				try
				{
					auto self(shared_from_this());

					// first wait for all send pending complete
					m_send_strand_ptr->post([this, self, prev_state]()
					{
						// asio don't allow operate the same socket in multi thread,if you close socket in one thread and another thread is 
						// calling socket's async_read... function,it will crash.so we must care for operate the socket.when need close the
						// socket ,we use the strand to post a event,make sure the socket's close operation is in the same thread.
						m_recv_strand_ptr->post([this, self, prev_state]()
						{
							// close the socket
							try
							{
								if (prev_state == state::running)
									_fire_close(get_last_error());

								// call listen socket's close function to notify the _handle_accept function response with error > 0 ,then the listen socket 
								// can get notify to exit
								if (m_socket.is_open())
								{
									m_socket.shutdown(asio::socket_base::shutdown_both);
									m_socket.close();
								}
							}
							catch (asio::system_error & e)
							{
								set_last_error(e.code().value());
							}

							m_state = state::stopped;
						});
					});
				}
				catch (std::exception &) {}
			}
		}

		/**
		 * @function : whether the sender is started
		 */
		virtual bool is_started() override
		{
			return ((m_state >= state::started) && m_socket.is_open());
		}

		/**
		 * @function : check whether the sender is stopped
		 */
		virtual bool is_stopped() override
		{
			return ((m_state == state::stopped) && !m_socket.is_open());
		}

		/**
		 * @function : send data
		 */
		virtual bool send(const std::string & ip, const std::string & port, std::shared_ptr<buffer<uint8_t>> buf_ptr) override
		{
			try
			{
				if (is_started() && !ip.empty() && !port.empty() && buf_ptr)
				{
					// must use strand.post to send data.why we should do it like this ? see udp_session._post_send.
					m_send_strand_ptr->post(std::bind(&udp_transmitter_impl::_post_send, this,
						shared_from_this(),
						ip,
						port,
						buf_ptr
					));
					return true;
				}
				else if (!m_socket.is_open())
				{
					set_last_error((int)errcode::socket_not_ready);
				}
				else
				{
					set_last_error((int)errcode::invalid_parameter);
				}
			}
			catch (asio::system_error & e)
			{
				set_last_error(e.code().value());
			}
			return false;
		}

	public:
		/**
		 * @function : get the socket shared_ptr
		 */
		inline asio::ip::udp::socket & get_socket()
		{
			return m_socket;
		}

		/**
		 * @function : get the local address
		 */
		virtual std::string get_local_address() override
		{
			try
			{
				if (m_socket.is_open())
				{
					return m_socket.local_endpoint().address().to_string();
				}
			}
			catch (asio::system_error & e)
			{
				set_last_error(e.code().value());
			}
			return std::string();
		}

		/**
		 * @function : get the local port
		 */
		virtual unsigned short get_local_port() override
		{
			try
			{
				if (m_socket.is_open())
				{
					return m_socket.local_endpoint().port();
				}
			}
			catch (asio::system_error & e)
			{
				set_last_error(e.code().value());
			}
			return 0;
		}

		/**
		 * @function : get the remote address
		 */
		virtual std::string get_remote_address() override
		{
			try
			{
				if (m_socket.is_open())
				{
					return m_socket.remote_endpoint().address().to_string();
				}
			}
			catch (asio::system_error & e)
			{
				set_last_error(e.code().value());
			}
			return std::string();
		}

		/**
		 * @function : get the remote port
		 */
		virtual unsigned short get_remote_port() override
		{
			try
			{
				if (m_socket.is_open())
				{
					return m_socket.remote_endpoint().port();
				}
			}
			catch (asio::system_error & e)
			{
				set_last_error(e.code().value());
			}
			return 0;
		}

	protected:
		virtual void _post_recv(std::shared_ptr<transmitter_impl> this_ptr, std::shared_ptr<buffer<uint8_t>> buf_ptr)
		{
			if (is_started())
			{
				if (buf_ptr->remain() > 0)
				{
					const auto & buffer = asio::buffer(buf_ptr->write_begin(), buf_ptr->remain());
					this->m_socket.async_receive_from(buffer, m_sender_endpoint,
						this->m_recv_strand_ptr->wrap(std::bind(&udp_transmitter_impl::_handle_recv, this,
							std::placeholders::_1, // error_code
							std::placeholders::_2, // bytes_recvd
							std::move(this_ptr),
							std::move(buf_ptr)
						)));
				}
				else
				{
					set_last_error((int)errcode::recv_buffer_size_too_small);
					ASIO2_DUMP_EXCEPTION_LOG_IMPL;
					this->stop();
					assert(false);
				}
			}
		}

		virtual void _handle_recv(const asio::error_code & ec, std::size_t bytes_recvd, std::shared_ptr<transmitter_impl> this_ptr, std::shared_ptr<buffer<uint8_t>> buf_ptr)
		{
			if (is_started())
			{
				auto use_count = buf_ptr.use_count();
				if (!ec)
				{
					// every times recv data,we update the last active time.
					this->reset_last_active_time();

					buf_ptr->write_bytes(bytes_recvd);

					_fire_recv(m_sender_endpoint, buf_ptr);
				}
				else
				{
					set_last_error(ec.value());

					if (ec == asio::error::operation_aborted)
						return;
				}

				if (use_count == buf_ptr.use_count())
				{
					buf_ptr->reset();
					this->_post_recv(std::move(this_ptr), std::move(buf_ptr));
				}
				else
				{
					this->_post_recv(std::move(this_ptr), std::make_shared<buffer<uint8_t>>(
						m_url_parser_ptr->get_recv_buffer_size(), malloc_recv_buffer(m_url_parser_ptr->get_recv_buffer_size()), 0));
				}
			}
		}

		virtual void _post_send(std::shared_ptr<transmitter_impl> this_ptr, const std::string & ip, const std::string & port, std::shared_ptr<buffer<uint8_t>> buf_ptr)
		{
			// the resolve function is a time-consuming operation,so we put the resolve in this work thread.
			asio::error_code ec;
			asio::ip::udp::resolver resolver(*m_recv_io_context_ptr);
			asio::ip::udp::resolver::query query(ip, port);
			asio::ip::udp::endpoint endpoint = *resolver.resolve(query, ec);

			if (ec)
			{
				set_last_error(ec.value());
				this->_fire_send(endpoint, buf_ptr, ec.value());
				ASIO2_DUMP_EXCEPTION_LOG_IMPL;
				return;
			}

			if (is_started())
			{
				m_socket.send_to(asio::buffer(buf_ptr->read_begin(), buf_ptr->size()), endpoint, 0, ec);
				set_last_error(ec.value());
				this->_fire_send(endpoint, buf_ptr, ec.value());
				if (ec)
				{
					ASIO2_DUMP_EXCEPTION_LOG_IMPL;
				}
			}
			else
			{
				set_last_error((int)errcode::socket_not_ready);
				this->_fire_send(endpoint, buf_ptr, get_last_error());
			}
		}

		virtual void _fire_recv(asio::ip::udp::endpoint & endpoint, std::shared_ptr<buffer<uint8_t>> & buf_ptr)
		{
			auto ip = endpoint.address().to_string();
			static_cast<sender_listener_mgr *>(m_listener_mgr_ptr.get())->notify_recv(ip, endpoint.port(), buf_ptr);
		}

		virtual void _fire_send(asio::ip::udp::endpoint & endpoint, std::shared_ptr<buffer<uint8_t>> & buf_ptr, int error)
		{
			auto ip = endpoint.address().to_string();
			static_cast<sender_listener_mgr *>(m_listener_mgr_ptr.get())->notify_send(ip, endpoint.port(), buf_ptr, error);
		}

		virtual void _fire_close(int error)
		{
			if (!m_fire_close_is_called.test_and_set(std::memory_order_acquire))
			{
				dynamic_cast<sender_listener_mgr *>(m_listener_mgr_ptr.get())->notify_close(error);
			}
		}

	protected:
		/// socket
		asio::ip::udp::socket m_socket;

		/// use to avoid call _fire_close twice
		std::atomic_flag m_fire_close_is_called = ATOMIC_FLAG_INIT;

		/// endpoint for udp 
		asio::ip::udp::endpoint m_sender_endpoint;

	};
}

#endif // !__ASIO2_UDP_TRANSMITTER_IMPL_HPP__
