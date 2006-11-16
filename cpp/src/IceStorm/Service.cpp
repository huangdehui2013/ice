// **********************************************************************
//
// Copyright (c) 2003-2006 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <IceStorm/TopicI.h>
#include <IceStorm/TopicManagerI.h>
#include <IceStorm/Instance.h>
#include <IceStorm/TraceLevels.h>
#include <IceStorm/BatchFlusher.h>
#include <IceStorm/SubscriberPool.h>
#include <IceStorm/Service.h>

using namespace std;
using namespace Ice;
using namespace IceStorm;
using namespace Freeze;

namespace IceStorm
{

class ServiceI : public ::IceStorm::Service
{
public:

    ServiceI();
    virtual ~ServiceI();

    virtual void start(const string&,
		       const CommunicatorPtr&,
		       const StringSeq&);

    virtual void start(const CommunicatorPtr&, 
		       const ObjectAdapterPtr&, 
		       const ObjectAdapterPtr&,
		       const string&, 
		       const Ice::Identity&,
		       const string&);

    virtual TopicManagerPrx getTopicManager() const;    

    virtual void stop();

private:

    TopicManagerIPtr _manager;
    TopicManagerPrx _managerProxy;
    InstancePtr _instance;
    ObjectAdapterPtr _topicAdapter;
    ObjectAdapterPtr _publishAdapter;
};

}

extern "C"
{

ICE_STORM_API ::IceBox::Service*
createIceStorm(CommunicatorPtr communicator)
{
    return new ServiceI;
}

}

ServicePtr
IceStorm::Service::create(const CommunicatorPtr& communicator,
			  const ObjectAdapterPtr& topicAdapter,
			  const ObjectAdapterPtr& publishAdapter,
			  const string& name,
			  const Ice::Identity& id,
			  const string& dbEnv)
{
    ServiceI* service = new ServiceI;
    ServicePtr svc = service;
    service->start(communicator, topicAdapter, publishAdapter, name, id, dbEnv);
    return svc;
}

IceStorm::ServiceI::ServiceI()
{
}

IceStorm::ServiceI::~ServiceI()
{
}

void
IceStorm::ServiceI::start(
    const string& name,
    const CommunicatorPtr& communicator,
    const StringSeq& args)
{
    PropertiesPtr properties = communicator->getProperties();

    _topicAdapter = communicator->createObjectAdapter(name + ".TopicManager");
    _publishAdapter = communicator->createObjectAdapter(name + ".Publish");

    //
    // We use the name of the service for the name of the database environment.
    //
    Identity topicManagerId;
    topicManagerId.category = properties->getPropertyWithDefault(name + ".InstanceName", "IceStorm");
    topicManagerId.name = "TopicManager";

    _instance = new Instance(name, communicator, _publishAdapter);
    
    try
    {
	_manager = new TopicManagerI(_instance, _topicAdapter, name, "topics");
	_managerProxy = TopicManagerPrx::uncheckedCast(_topicAdapter->add(_manager, topicManagerId));
    }
    catch(const Ice::Exception&)
    {
	_instance->destroy();
	_instance = 0;
	throw;
    }
	
    _topicAdapter->activate();
    _publishAdapter->activate();

    //
    // The keep alive thread must be started after all topics are
    // installed so that any upstream topics are notified immediately
    // after startup.
    //
    //_instance->keepAlive()->startPinging();
}

void
IceStorm::ServiceI::start(const CommunicatorPtr& communicator,
			  const ObjectAdapterPtr& topicAdapter,
			  const ObjectAdapterPtr& publishAdapter,
			  const string& name,
			  const Ice::Identity& id,
			  const string& dbEnv)
{
    _instance = new Instance(name, communicator, publishAdapter);

    //
    // We use the name of the service for the name of the database environment.
    //
    try
    {
	_manager = new TopicManagerI(_instance, topicAdapter, dbEnv, "topics");
	_managerProxy = TopicManagerPrx::uncheckedCast(topicAdapter->add(_manager, id));
    }
    catch(const Ice::Exception&)
    {
	_instance->destroy();
	_instance = 0;
	throw;
    }
}

TopicManagerPrx
IceStorm::ServiceI::getTopicManager() const
{
    return _managerProxy;
}

void
IceStorm::ServiceI::stop()
{
    if(_topicAdapter)
    {
	_topicAdapter->deactivate();
    }
    if(_publishAdapter)
    {
	_publishAdapter->deactivate();
    }

    //
    // Instance::shutdown terminates all the thread pools, however, it
    // does not clear the references. This is because the shutdown has
    // to be in two stages. First we destroy & join with the threads
    // so that no further activity can take place. Then we reap()
    // which has to call on various instance objects (such as the keep
    // alive thread), then we clear the instance which breaks any
    // cycles.
    //

    //
    // Shutdown the instance.
    //
    _instance->shutdown();

    //
    // It's necessary to reap all destroyed topics on shutdown.
    //
    _manager->shutdown();

    //
    // ... and finally destroy the instance.
    //
    _instance->destroy();
}
