#pragma once
#include <vector>
#include <thread>
#include <memory>
#include <functional>
#include <atomic>
#include <map>
#include <condition_variable>
#include <zmq.hpp>
#include "zhelpers.hpp"
#include "zmsg.hpp"
#include <memory>
#include <util.hpp>
// for test, delete later
#include <unistd.h>

class worker_base
{
  public:
    worker_base()
        : ctx_(1),
          worker_socket_(ctx_, ZMQ_DEALER), uniqueID_atomic(1)
    {
        IP_and_port_dest = "127.0.0.1:5560";
        monitor_cb = NULL;
        routine_thread = NULL;
        monitor_thread = NULL;
        should_exit_monitor_task = false;
        should_exit_routine_task = false;
    }

    ~worker_base()
    {
#if 0
        if (worker_socket_)
        {
            delete worker_socket_;
        }
        if (ctx_)
        {
            delete ctx_;
        }
#endif
        should_exit_monitor_task = true;
        should_exit_routine_task = true;
        if (routine_thread)
        {
            routine_thread->join();
        }
        if (monitor_thread)
        {
            monitor_thread->join();
        }
    }

    bool run()
    {
        auto routine_fun = std::bind(&worker_base::start, this);
        routine_thread = new std::thread(routine_fun);
        //routine_thread->detach();
        auto monitor_fun = std::bind(&worker_base::monitor_task, this);
        monitor_thread = new std::thread(monitor_fun);
        //monitor_thread->detach();
        // start monitor socket
        bool ret = monitor_this_socket();
        if (ret)
        {
        }
        else
        {
            // log here, start monitor socket fail!
            return false;
        }
    }
    void setIPPort(std::string ipport)
    {
        IP_and_port_dest = ipport;
    }
    std::string getIPPort()
    {
        return IP_and_port_dest;
    }
    void setIPPortSource(std::string ipport)
    {
        IP_and_port_source = ipport;
    }
    std::string getIPPortSource()
    {
        return IP_and_port_source;
    }
    void set_cb(SERVER_CB_FUNC cb)
    {
        if (cb)
        {
            cb_ = cb;
        }
        else
        {
            //log here
        }
    }

    size_t send(const char *msg, size_t len, void *ID)
    {
        auto iter = Id2MsgMap.find(ID);
        if (iter != Id2MsgMap.end())
        {
            zmsg::ustring tmp_ustr((unsigned char *)msg, len);
            // to do add the send code
            (iter->second)->push_back(tmp_ustr);
            (iter->second)->send(worker_socket_);
            // make sure delete the memory of the message
            //(iter->second)->clear();
            (iter->second).reset();
            Id2MsgMap.erase(iter);
        }
        else
        {
            // log here, did not find the ID
            return -1;
        }
    }

    void set_monitor_cb(MONITOR_CB_FUNC cb)
    {
        if (cb)
        {
            monitor_cb = cb;
        }
        else
        {
            //log here
        }
    }

    void *getUniqueID() { return (void *)(uniqueID_atomic++); };

  private:
    void epoll_task()
    {
#if 0
        std::unique_lock<std::mutex> monitor_lock(monitor_mutex);
        // to do, receive signal then do other thing.
        // if signal timeout, that means routine thread is abnormal. Or exit. start again.
        while (1)
        {
            {
                //monitor_cond.wait(monitor_lock);
                if (monitor_cond.wait_for(dnsLock, std::chrono::milliseconds(EPOLL_TIMEOUT + 5000)) == std::cv_status::timeout)
                {
                    // timeout waitting for signal. there must be something wrong with epoll
                    auto routine_fun = std::bind(&worker_base::start, this);
                    std::thread routine_thread(routine_fun);
                    routine_thread.detach();
                }
            }
        }
#endif
    }

    bool monitor_task()
    {
        void *server_mon = zmq_socket((void *)ctx_, ZMQ_PAIR);
        if (!server_mon)
        {
            // log here
            return false;
        }
        try
        {
            int rc = zmq_connect(server_mon, "inproc://monitor-worker");

            //rc should be 0 if success
            if (rc)
            {
                //
                return false;
            }
        }
        catch (std::exception &e)
        {
            logger->error(ZMQ_LOG, "connect to monitor worker socket fail\n");
            return false;
        }

        while (1)
        {
            if (should_exit_monitor_task)
            {
                return true;
            }
            std::string address;
            int value;
            int event = get_monitor_event(server_mon, &value, address);
            if (event == -1)
            {
                return false;
            }

            //std::cout << "receive event form server monitor task, the event is " << event << ". Value is : " << value << ". string is : " << address << std::endl;
            if (monitor_cb)
            {
                monitor_cb(event, value, address);
            }
        }
    }
    bool monitor_this_socket()
    {
        int rc = zmq_socket_monitor(worker_socket_, "inproc://monitor-worker", ZMQ_EVENT_ALL);
        return ((rc == 0) ? true : false);
    }
    size_t send(zmsg &input)
    {
        input.send(worker_socket_);
    }
    size_t send(const char *msg, size_t len)
    {
        worker_socket_.send(msg, len);
    }
    bool start()
    {

        // enable IPV6, we had already make sure that we are using TCP then we can set this option
        int enable_v6 = 1;
        if (zmq_setsockopt(worker_socket_, ZMQ_IPV6, &enable_v6, sizeof(enable_v6)) < 0)
        {
            worker_socket_.close();
            ctx_.close();
            return false;
        }
        /*
        // generate random identity
        char identity[10] = {};
        sprintf(identity, "%04X-%04X", within(0x10000), within(0x10000));
        printf("%s\n", identity);
        worker_socket_.setsockopt(ZMQ_IDENTITY, identity, strlen(identity));
        */
        std::string identity = s_set_id(worker_socket_);
        logger->debug(ZMQ_LOG, "\[WORKER\] set ID %s to worker\n", identity.c_str());

        int linger = 0;
        if (zmq_setsockopt(worker_socket_, ZMQ_LINGER, &linger, sizeof(linger)) < 0)
        {
            worker_socket_.close();
            ctx_.close();
            return false;
        }
        /*
        - Change the ZMQ_TIMEOUT?for ZMQ_RCVTIMEO and ZMQ_SNDTIMEO.
        - Value is an uint32 in ms (to be compatible with windows and kept the
        implementation simple).
        - Default to 0, which would mean block infinitely.
        - On timeout, return EAGAIN.
        Note: Maxx will this work for DEALER mode?
        */
        int iRcvSendTimeout = 5000; // millsecond Make it configurable

        if (zmq_setsockopt(worker_socket_, ZMQ_RCVTIMEO, &iRcvSendTimeout, sizeof(iRcvSendTimeout)) < 0)
        {
            worker_socket_.close();
            ctx_.close();
            return false;
        }
        if (zmq_setsockopt(worker_socket_, ZMQ_SNDTIMEO, &iRcvSendTimeout, sizeof(iRcvSendTimeout)) < 0)
        {
            worker_socket_.close();
            ctx_.close();
            return false;
        }

        try
        {
            std::string IPPort;
            // should be like this tcp://192.168.1.17:5555;192.168.1.1:5555

            if (IP_and_port_source.empty())
            {
                IPPort += "tcp://" + IP_and_port_dest;
            }
            else
            {
                IPPort += "tcp://" + IP_and_port_source + ";" + IP_and_port_dest;
            }

            logger->debug(ZMQ_LOG, "\[WORKER\] connect to : %s\n", IPPort.c_str());
            worker_socket_.connect(IPPort);
        }
        catch (std::exception &e)
        {
            logger->error(ZMQ_LOG, "\[WORKER\] connect fail!!!!\n");
            // log here, connect fail
            return false;
        }
        // tell the broker that we are ready
        std::string ready_str("READY");
        send(ready_str.c_str(), ready_str.size());

        //  Initialize poll set
        zmq::pollitem_t items[] = {
            {worker_socket_, 0, ZMQ_POLLIN, 0}};
        while (1)
        {
            if (should_exit_routine_task)
            {
                return true;
            }
            try
            {
                // by default we wait for 500ms then so something. like hreatbeat
                zmq::poll(items, 1, -1);
                if (items[0].revents & ZMQ_POLLIN)
                {

                    // this is for test, delete it later
                    //sleep(5);

                    zmsg_ptr msg(new zmsg(worker_socket_));
                    logger->debug(ZMQ_LOG, "\[WORKER\] get message from broker with %d part", msg->parts());
                    msg->dump();
                    // now we get the message .
                    // this is the normal message
                    if (msg->parts() == 3)
                    {
                        std::string data = msg->get_body();
                        if (data.empty())
                        {
                            // log here, we get a message without body
                            continue;
                        }
                        void *ID = getUniqueID();
                        Id2MsgMap.emplace(ID, msg);

                        // ToDo: now we got the message, do main work
                        //std::cout << "receive message form client" << std::endl;
                        //msg.dump();
                        // send back message to client, for test
                        //msg.send(worker_socket_);
                        if (cb_)
                        {
                            cb_(data.c_str(), data.size(), ID);
                        }
                        else
                        {
                            //log here as there is no callback function
                        }
                    }
                    else
                    {
                        // to do , do some other work, eg: check if the heartbeat is lost
                    }
                    // to do : add some code eg:check if it is the time that we should send heartbeat
                }
                else
                {
                    std::cout << " epoll timeout !" << std::endl;
                    // to do, signal the monitor thread
                }
            }
            catch (std::exception &e)
            {
                //std::unique_lock<std::mutex> monitor_lock(monitor_mutex);
                //monitor_cond.notify_all();
                //return false;
            }
        }
    }

  private:
    // this is for test
    //int seq_num;
    SERVER_CB_FUNC *cb_;
    MONITOR_CB_FUNC *monitor_cb;
    std::string IP_and_port_dest;
    std::string IP_and_port_source;
    zmq::context_t ctx_;
    zmq::socket_t worker_socket_;
    std::atomic<long> uniqueID_atomic;
    std::map<void *, zmsg_ptr> Id2MsgMap;

    std::condition_variable monitor_cond;
    std::mutex monitor_mutex;

    std::thread *routine_thread;
    std::thread *monitor_thread;
    bool should_exit_monitor_task;
    bool should_exit_routine_task;
};