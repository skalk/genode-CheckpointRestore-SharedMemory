/*
 * \brief  Checkpoint component
 * \author Denis Huber
 * \date   2016-09-17
 */

/* Genode includes */
#include <base/env.h>
#include <base/log.h>
#include <base/component.h>
#include <base/rpc_server.h>
#include <base/signal.h>
#include <base/heap.h>
#include <root/component.h>
#include <rm_session/connection.h>
#include <timer_session/connection.h>
#include <region_map/client.h>
#include <cpu_thread/client.h>

/* Resource includes */
#include <resource_session/resource_session.h>

namespace Resource {
	struct Client_resources;
	class  Fault_handler;
	class  Session_component;
	class  Root;
	struct Main;
}

/**
 * \brief Resources of client
 *
 * These resources are used to pause/resume the client's main thread
 * and attach/detach the sub dataspaces of the region map provided to
 * the client as a managed dataspace
 */
struct Resource::Client_resources : public Genode::List<Client_resources>::Element
{
	Genode::Thread_capability              thread_cap;
	Genode::Rm_connection                  rm_service;
	Genode::size_t                         rm_size;
	/**
	 * Region map of the managed dataspace
	 */
	Genode::Region_map_client              rm_client;
	Genode::size_t                         size0;
	Genode::size_t                         size1;
	/**
	 * Designated dataspaces for the region map
	 */
	Genode::Dataspace_capability           sub_ds_cap0;
	Genode::Dataspace_capability           sub_ds_cap1;
	Genode::addr_t                         addr0;
	Genode::addr_t                         addr1;
	bool                                   attached0;
	bool                                   attached1;

	Client_resources(Genode::Env &env)
	:
		thread_cap  (),
		rm_service  (env),
		rm_size     (8*1024),
		rm_client   (rm_service.create(8*1024)),
		size0       (4*1024),
		size1       (4*1024),
		sub_ds_cap0 (env.ram().alloc(size0)),
		sub_ds_cap1 (env.ram().alloc(size1)),
		addr0       (0),
		addr1       (4*1024),
		attached0   (false),
		attached1   (false)
	{ }

};


/**
 * Implementation of the session object for sharing resources
 *
 * Thread capability is transfered from client to this server
 * Dataspace capability is provided by the server (as a managed dataspace)
 */
class Resource::Session_component : public Genode::Rpc_object<Session>
{
private:
	Client_resources &_cli_res;

public:
	Session_component(Client_resources &cli_res)
	:
		_cli_res(cli_res)
	{ }

	void provide(Genode::Native_capability thread_cap, Genode::uint32_t)
	{
		_cli_res.thread_cap = Genode::reinterpret_cap_cast<Genode::Cpu_thread>(thread_cap);
	}
	Genode::Native_capability request(Genode::uint32_t)
	{
		return _cli_res.rm_client.dataspace();
	}
};

class Resource::Root : public Genode::Root_component<Session_component>
{
private:
	Client_resources &_cli_res;
protected:
	Session_component *_create_session(const char *args)
	{
		return new (md_alloc()) Session_component(_cli_res);
	}

public:
	Root(Genode::Entrypoint &session_ep, Genode::Allocator &md_alloc, Client_resources &cli_res)
	:
		Root_component<Session_component>(session_ep, md_alloc), _cli_res(cli_res)
	{ }
};

struct Resource::Main
{
	Client_resources             cli_res;
	Genode::Sliced_heap          sliced_heap;
	Genode::Entrypoint           session_ep;
	Resource::Root               root;
	Genode::Signal_handler<Main> fault_handler;
	Timer::Connection            timer;
	unsigned                     i = 0;
	Genode::Signal_handler<Main> timer_handler;

	void handle_fault()
	{
		Genode::Region_map::State state = cli_res.rm_client.state();

		Genode::log("Handling page fault: ",
				state.type == Genode::Region_map::State::READ_FAULT  ? "READ_FAULT"  :
				state.type == Genode::Region_map::State::WRITE_FAULT ? "WRITE_FAULT" :
				state.type == Genode::Region_map::State::EXEC_FAULT  ? "EXEC_FAULT"  : "READY",
				" pf_addr=", Genode::Hex(state.addr));

		// Pf in first designated dataspace
		if(state.addr >= cli_res.addr0 && state.addr < cli_res.addr0 + cli_res.size0)
		{
			void *att_addr = cli_res.rm_client.attach_at(cli_res.sub_ds_cap0, cli_res.addr0);
			cli_res.attached0 = true;
			Genode::log("  attached sub_ds0 at address ", att_addr);
		}
		// Pf in second designated dataspace
		else if(state.addr >= cli_res.addr1 && state.addr < cli_res.addr1 + cli_res.size1)
		{
			void *att_addr = cli_res.rm_client.attach_at(cli_res.sub_ds_cap1, cli_res.addr1);
			cli_res.attached1 = true;
			Genode::log("  attached sub_ds1 at address ", att_addr);
		}
		// Pf elsewhere (should never happen)
		else
		{
			Genode::error("invalid page fault address");
		}
	}

	void handle_timer()
	{
		using namespace Genode;

		log("Iteration #", i);
		if(cli_res.thread_cap.valid())
		{
			log("  valid thread");
			//Genode::Thread_state ts = Genode::Cpu_thread_client{cli_res.thread_cap}.state();
			//log(Genode::Hex(ts.cpu_exception));
			log("  pausing thread");
			Genode::Cpu_thread_client{cli_res.thread_cap}.pause();
			//ts = Genode::Cpu_thread_client{cli_res.thread_cap}.state();
			//log(Genode::Hex(ts.cpu_exception));

			if(cli_res.attached0)
			{
				log("  detaching sub_ds_cap0");
				cli_res.rm_client.detach(cli_res.addr0);
				cli_res.attached0 = false;
			}
			else
			{
				log("  sub_ds_cap0 already detached");
			}

			if(cli_res.attached1)
			{
				log("  detaching sub_ds_cap1");
				cli_res.rm_client.detach(cli_res.addr1);
				cli_res.attached1 = false;
			}
			else
			{
				log("  sub_ds_cap1 already detached");
			}

			log("  resuming thread");
			Genode::Cpu_thread_client{cli_res.thread_cap}.resume();
			//ts = Genode::Cpu_thread_client{cli_res.thread_cap}.state();
			//log(Genode::Hex(ts.cpu_exception));
		}
		else
		{
			log("  invalid thread");
		}
		i++;
	}

	Main(Genode::Env &env)
	:
		cli_res       (env),
		sliced_heap   (env.ram(), env.rm()),
		session_ep    (env, 16*1024, "resource_session_ep"),
		root          (session_ep, sliced_heap, cli_res),
		fault_handler (env.ep(), *this, &Main::handle_fault),
		timer         (env),
		timer_handler (env.ep(), *this, &Main::handle_timer)
	{
		using Genode::log;

		log("Initialization started");
		cli_res.rm_client.fault_handler(fault_handler);
		timer.sigh(timer_handler);

		log("Announcing Resource service");
		env.parent().announce(session_ep.manage(root));

		log("Initialization ended");
		log("Starting main loop");
		timer.trigger_periodic(4000*1000);
	}
};

Genode::size_t Component::stack_size() { return 32*1024; }

void Component::construct(Genode::Env &env)
{
	static Resource::Main main {env};
}
