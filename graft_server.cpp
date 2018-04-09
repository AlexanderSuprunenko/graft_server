#include "mongoose.h"

#include <memory>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <deque>

#include "router.h"

#include "thread_pool.h"

/*
////////
graft_server.cpp
+ mongoose.c
+ mongoose.h
 /mongoose
   CMakeLists.txt
 /thread-pool
   CMakeLists.txt
 /r3
   CMakeLists.txt
*/

class manager_t;

class ClientRequest;
using ClientRequest_ptr = std::shared_ptr<ClientRequest>;

class CryptoNodeSender;
//using CryptoNodeSender_ptr = std::shared_ptr<CryptoNodeSender>;

//////////////////////////////
class manager_t;

class GJ_ptr;
using TPResQueue = MPMCBoundedQueue< GJ_ptr >;

using GJ = GraftJob<ClientRequest_ptr, Router::JobParams, TPResQueue, manager_t, std::string>;

//////////////
/// \brief The GJ_ptr class
/// A wrapper of GraftJob that will be moved from queue to queue with fixed size.
/// It contains single data member of unique_ptr
///
class GJ_ptr final
{
	std::unique_ptr<GJ> ptr = nullptr;
public:
	GJ_ptr(GJ_ptr&& rhs)
	{
		*this = std::move(rhs);
	}
	GJ_ptr& operator = (GJ_ptr&& rhs)
	{
//		if(this != &rhs)
		{
			ptr = std::move(rhs.ptr);
		}
		return * this;
	}

//	GJ_ptr() = delete;
	explicit GJ_ptr() = default;
	GJ_ptr(const GJ_ptr&) = delete;
	GJ_ptr& operator = (const GJ_ptr&) = delete;
	~GJ_ptr() = default;

	template<typename ...ARGS>
	GJ_ptr(ARGS&&... args) : ptr( new GJ( std::forward<ARGS>(args)...) )
	{
	}

	template<typename ...ARGS>
	void operator ()(ARGS... args)
	{
		ptr.get()->operator () (args...);
	}

	GJ* operator ->()
	{
		return ptr.operator ->();
	}

	GJ& operator *()
	{
		return ptr.operator *();
	}
};

using ThreadPoolX = ThreadPoolImpl<FixedFunction<void(), sizeof(GJ_ptr)>,
								  MPMCBoundedQueue>;

///////////////////////////////////

class manager_t
{
	mg_mgr mgr;

	int cntClientRequest = 0;
	int cntClientRequestDone = 0;
	int cntCryptoNodeSender = 0;
	int cntCryptoNodeSenderDone = 0;
	int cntJobDone = 0;

	std::unique_ptr<ThreadPoolX> threadPool;
	std::unique_ptr<TPResQueue> resQueue;
public:
	mg_mgr* get_mg_mgr() { return &mgr; }
	ThreadPoolX& get_threadPool() { return *threadPool.get(); }
	TPResQueue& get_resQueue() { return *resQueue.get(); }
	
	void DoWork();

	void OnNewClient(ClientRequest_ptr cr)
	{
		++cntClientRequest;
		static bool b = false;
		if(b)
		{
			SendCrypton(cr);
		}
		else
		{
			SendToThreadPool(cr);
		}
		b = !b;
	}
	void OnClientDone(ClientRequest_ptr cr)
	{
		++cntClientRequestDone;
	}

	void OnCryptonDone(CryptoNodeSender* cns);
public:

	void setThreadPool(ThreadPoolX&& tp, TPResQueue&& rq)
	{
		threadPool = std::unique_ptr<ThreadPoolX>(new ThreadPoolX(std::move(tp)));
		resQueue = std::unique_ptr<TPResQueue>(new TPResQueue(std::move(rq)));
	}
	
	void notifyJobReady()
	{
		mg_notify(&mgr);
	}

public:
	void SendCrypton(ClientRequest_ptr cr);
	void SendToThreadPool(ClientRequest_ptr cr);

	static void cb_event(mg_mgr* mgr, uint64_t val);
};

manager_t manager;

void manager_t::cb_event(mg_mgr* mgr, uint64_t val)
{
	manager.DoWork();
}


template<typename C>
class StaticMongooseHandler
{
public:	
	static void static_ev_handler(mg_connection *nc, int ev, void *ev_data) 
	{
		static bool entered = false;
		assert(!entered); //recursive calls are dangerous
		entered = true;
		C* This = static_cast<C*>(nc->user_data);
		assert(This);
		This->ev_handler(nc, ev, ev_data);
		entered = false;
	}
	static void static_empty_ev_handler(mg_connection *nc, int ev, void *ev_data)
	{

	}
};

template<typename C>
class ItselfHolder
{
public:	
	using ptr = std::shared_ptr<C>;
private:	
	ptr itself;
protected:	
	ItselfHolder() : itself(static_cast<C*>(this)) { }
public:	
	ptr getItself() { return itself; }
	
	template<typename ...ARGS>
	static const ptr Create(ARGS&&... args)
	{
		return (new C(std::forward<ARGS>(args)...))->itself;
	}
	void Release() { itself.reset(); }
};

class CryptoNodeSender : public ItselfHolder<CryptoNodeSender>, StaticMongooseHandler<CryptoNodeSender>
{
	mg_connection *crypton = nullptr;
public:	
	ClientRequest_ptr cr;
	std::string data;
	std::string result;
	
	CryptoNodeSender() = default;
	
	void send(ClientRequest_ptr cr_, std::string& data_)
	{
		cr = cr_;
		data = data_;
		crypton = mg_connect(manager.get_mg_mgr(),"localhost:1234", static_ev_handler);
		crypton->user_data = this;
		mg_send(crypton, data.c_str(), data.size());
	}
public:	
	void ev_handler(mg_connection* crypton, int ev, void *ev_data) 
	{
		assert(crypton == this->crypton);
		switch (ev) 
		{
		case MG_EV_RECV:
		{
			int cnt = *(int*)ev_data;
			if(cnt<100) break;
			mbuf& buf = crypton->recv_mbuf;
			result = std::string(buf.buf, buf.len);
			crypton->flags |= MG_F_CLOSE_IMMEDIATELY;
			manager.OnCryptonDone(this);
			crypton->handler = static_empty_ev_handler;
			Release();
		} break;
		default:
		  break;
		}
	}
};

class ClientRequest : public ItselfHolder<ClientRequest>, public StaticMongooseHandler<ClientRequest>
{
	enum class State
	{
		None,
		ToCrytonode,
		ToThreadPool,
		AnswerError,
		Delete,
//		Stop
	};
	
	Router::JobParams prms;
	State state;
	mg_connection *client;
public:	
	
	State get_state(){ return state; }
	State set_state(State s){ state = s; }
private:
	friend class ItselfHolder<ClientRequest>;
	ClientRequest(mg_connection *client, Router::JobParams& prms) 
		: prms(prms)
		, state(State::None)
		, client(client)
	{
	}
public:	
	void AnswerOk()
	{
		std::string s("I am answering Ok");
		mg_send(client, s.c_str(), s.size());
		client->flags |= MG_F_SEND_AND_CLOSE;
		client->handler = static_empty_ev_handler;
		Release();
	}
	
	void CreateJob()
	{
		manager.get_threadPool().post(
			GJ_ptr( getItself(), Router::JobParams(prms), &manager.get_resQueue(), &manager )
		);
	}

	void JobDone(GJ&& gj)
	{
		std::string s("Job done");
		mg_send(client, s.c_str(), s.size());
		client->flags |= MG_F_SEND_AND_CLOSE;
		client->handler = static_empty_ev_handler;
		Release();
	}
	
public:
	
	void ev_handler(mg_connection *client, int ev, void *ev_data) 
	{
		assert(client == this->client);
		switch (ev) 
		{
		case MG_EV_POLL:
		{
			if(state == State::ToCrytonode)
			{
				
			}
		} break;
		case MG_EV_CLOSE:
		{
			assert(getItself());
			if(getItself()) break;
			state = State::Delete;
			manager.OnClientDone(getItself());
			client->handler = static_empty_ev_handler;
			Release();
		} break;
		default:
		  break;
		}
	}
};

class GraftServer final
{
	static Router router;
public:	
	static Router& get_router() { return router; }
	GraftServer()
	{ }
	
	void serve(const char* s_http_port)
	{
		mg_mgr& mgr = *manager.get_mg_mgr();
		mg_mgr_init(&mgr, NULL, manager.cb_event);
		mg_connection* nc = mg_bind(&mgr, s_http_port, ev_handler);
		mg_set_protocol_http_websocket(nc);
		for (;;) 
		{
//			mg_mgr_poll(&mgr, 1000);
			mg_mgr_poll(&mgr, -1);
		}
		mg_mgr_free(&mgr);
	}

private:
	static void ev_handler(mg_connection *client, int ev, void *ev_data) 
	{
		switch (ev) 
		{
		case MG_EV_HTTP_REQUEST:
		{
			struct http_message *hm = (struct http_message *) ev_data;
			std::string uri(hm->uri.p, hm->uri.len);
			std::string s_method(hm->method.p, hm->method.len);
			int method = (s_method == "GET")? METHOD_GET: 1;
			
			Router::JobParams prms;
			if(router.match(uri, method, prms))
			{
				ClientRequest* ptr = ClientRequest::Create(client, prms).get();
				client->user_data = ptr;
				client->handler = ClientRequest::static_ev_handler;
				//short or long?
				//if(short) exec
				//
				//if to cropton
				
				manager.OnNewClient( ptr->getItself() );
			}
			else
			{
				mg_http_send_error(client, 500, "invalid parameter");
				client->flags |= MG_F_SEND_AND_CLOSE;
			}
		} break;
		default:
		  break;
		}
	}
};

Router GraftServer::router;

void manager_t::SendCrypton(ClientRequest_ptr cr)
{
	++cntCryptoNodeSender;
	CryptoNodeSender::ptr cns = CryptoNodeSender::Create();
	std::string something(100, ' ');
	{
		std::string s("something");
		for(int i=0; i< s.size(); ++i)
		{
			something[i] = s[i];
		}
	}
	cns->send(cr, something );
}

void manager_t::SendToThreadPool(ClientRequest_ptr cr)
{
	cr->CreateJob();
}

void manager_t::DoWork()
{
	GJ_ptr gj;
	bool res = manager.get_resQueue().pop(gj);
	assert(res);
	gj->cr->JobDone(std::move(*gj));
	++cntJobDone;
}

void manager_t::OnCryptonDone(CryptoNodeSender* cns)
{
	++cntCryptoNodeSenderDone;
	cns->cr->AnswerOk();
}

class cryptoNodeServer
{
public:
	static void run()
	{
		mg_mgr mgr;
		mg_mgr_init(&mgr, NULL, 0);
		mg_connection *nc = mg_bind(&mgr, "1234", ev_handler);
		for (;;) {
		  mg_mgr_poll(&mgr, 1000);
		}
		mg_mgr_free(&mgr);
	}
private:
	static void ev_handler(mg_connection *client, int ev, void *ev_data) 
	{
		switch (ev) 
		{
		case MG_EV_RECV:
		{
			int cnt = *(int*)ev_data;
			if(cnt<100) break;
			mbuf& buf = client->recv_mbuf;
			static std::string data = std::string(buf.buf, buf.len);
			mg_send(client, data.c_str(), data.size());
			client->flags |= MG_F_SEND_AND_CLOSE;
		} break;
		default:
		  break;
		}
	}
};

#include<thread>

bool test(Router::vars_t& vars, const std::string& input, std::string& output)
{
	return true;
}

void init_threadPool()
{
	ThreadPoolOptions th_op;
//		th_op.setThreadCount(3);
	th_op.setQueueSize(32);
//		th_op.setQueueSize(4);
	ThreadPoolX thread_pool(th_op);

	size_t resQueueSize;
	{//nearest ceiling power of 2
		size_t val = th_op.threadCount()*th_op.queueSize();
		size_t bit = 1;
		for(; bit<val; bit <<= 1);
		resQueueSize = bit;
	}

	const size_t maxinputSize = th_op.threadCount()*th_op.queueSize();
	assert(maxinputSize == th_op.threadCount()*th_op.queueSize());
	TPResQueue resQueue(resQueueSize);
	
	manager.setThreadPool(std::move(thread_pool), std::move(resQueue));
}

int main(int argc, char *argv[]) 
{
	std::thread t(cryptoNodeServer::run);
	
	GraftServer gs; //router);
	init_threadPool();
	{
		static Router::Handler p = test;
		Router& router = gs.get_router();
		router.addRoute("/root/r{id:\\d+}", METHOD_GET, &p);
		router.addRoute("/root/aaa/{s1}/bbb/{s2}", METHOD_GET, &p);
//		router.addRoute("/root/rr{id:\\d+}", METHOD_GET, test);
//		router.addRoute("/root/aaaaaa/{s1}/bbbbbb/{s2}", METHOD_GET, test);
		bool res = router.arm();
		assert(res);
	}
	gs.serve("9084");
	
	t.join();
	return 0;
}

