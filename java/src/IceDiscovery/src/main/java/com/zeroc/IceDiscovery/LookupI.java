// **********************************************************************
//
// Copyright (c) 2003-2017 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

package com.zeroc.IceDiscovery;

import java.util.List;
import java.util.ArrayList;
import java.util.Map;
import java.util.HashMap;
import java.util.concurrent.CompletableFuture;

class LookupI implements Lookup
{
    abstract private class Request<T, Ret> implements Runnable
    {
        Request(T id, int retryCount)
        {
            _id = id;
            _nRetry = retryCount;
        }

        T getId()
        {
            return _id;
        }

        boolean addFuture(CompletableFuture<Ret> f)
        {
            _futures.add(f);
            return _futures.size() == 1;
        }

        boolean retry()
        {
            return --_nRetry >= 0;
        }

        void scheduleTimer(long timeout)
        {
            _future = _timer.schedule(this, timeout, java.util.concurrent.TimeUnit.MILLISECONDS);
        }

        void cancelTimer()
        {
            assert _future != null;
            _future.cancel(false);
            _future = null;
        }

        protected int _nRetry;
        protected List<CompletableFuture<Ret>> _futures = new ArrayList<>();
        private T _id;
        protected java.util.concurrent.Future<?> _future;
    }

    private class AdapterRequest extends Request<String, com.zeroc.Ice.ObjectPrx>
    {
        AdapterRequest(String id, int retryCount)
        {
            super(id, retryCount);
            _start = System.nanoTime();
            _latency = 0;
        }

        @Override
        boolean retry()
        {
            return _proxies.size() == 0 && --_nRetry >= 0;
        }

        boolean response(com.zeroc.Ice.ObjectPrx proxy, boolean isReplicaGroup)
        {
            if(isReplicaGroup)
            {
                _proxies.add(proxy);
                if(_latency == 0)
                {
                    _latency = (long)((System.nanoTime() - _start) * _latencyMultiplier / 100000.0);
                    if(_latency == 0)
                    {
                        _latency = 1; // 1ms
                    }
                    cancelTimer();
                    scheduleTimer(_latency);
                }
                return false;
            }
            finished(proxy);
            return true;
        }

        void finished(com.zeroc.Ice.ObjectPrx proxy)
        {
            if(proxy != null || _proxies.isEmpty())
            {
                sendResponse(proxy);
                return;
            }
            else if(_proxies.size() == 1)
            {
                sendResponse(_proxies.get(0));
                return;
            }

            List<com.zeroc.Ice.Endpoint> endpoints = new ArrayList<>();
            com.zeroc.Ice.ObjectPrx result = null;
            for(com.zeroc.Ice.ObjectPrx prx : _proxies)
            {
                if(result == null)
                {
                    result = prx;
                }
                endpoints.addAll(java.util.Arrays.asList(prx.ice_getEndpoints()));
            }
            sendResponse(result.ice_endpoints(endpoints.toArray(new com.zeroc.Ice.Endpoint[endpoints.size()])));
        }

        @Override
        public void run()
        {
            adapterRequestTimedOut(this);
        }

        private void sendResponse(com.zeroc.Ice.ObjectPrx proxy)
        {
            for(CompletableFuture<com.zeroc.Ice.ObjectPrx> f : _futures)
            {
                f.complete(proxy);
            }
            _futures.clear();
        }

        private List<com.zeroc.Ice.ObjectPrx> _proxies = new ArrayList<>();
        private long _start;
        private long _latency;
    }

    private class ObjectRequest extends Request<com.zeroc.Ice.Identity, com.zeroc.Ice.ObjectPrx>
    {
        ObjectRequest(com.zeroc.Ice.Identity id, int retryCount)
        {
            super(id, retryCount);
        }

        void response(com.zeroc.Ice.ObjectPrx proxy)
        {
            finished(proxy);
        }

        void finished(com.zeroc.Ice.ObjectPrx proxy)
        {
            for(CompletableFuture<com.zeroc.Ice.ObjectPrx> f : _futures)
            {
                f.complete(proxy);
            }
            _futures.clear();
        }

        @Override
        public void run()
        {
            objectRequestTimedOut(this);
        }
    }

    public LookupI(LocatorRegistryI registry, LookupPrx lookup, com.zeroc.Ice.Properties properties)
    {
        _registry = registry;
        _timeout = properties.getPropertyAsIntWithDefault("IceDiscovery.Timeout", 300);
        _retryCount = properties.getPropertyAsIntWithDefault("IceDiscovery.RetryCount", 3);
        _latencyMultiplier = properties.getPropertyAsIntWithDefault("IceDiscovery.LatencyMultiplier", 1);
        _domainId = properties.getProperty("IceDiscovery.DomainId");
        _timer = com.zeroc.IceInternal.Util.getInstance(lookup.ice_getCommunicator()).timer();

        try
        {
            lookup.ice_getConnection();
        }
        catch(com.zeroc.Ice.LocalException ex)
        {
            StringBuilder b = new StringBuilder();
            b.append("IceDiscovery is unable to establish a multicast connection:\n");
            b.append("proxy = ");
            b.append(lookup.toString());
            b.append('\n');
            b.append(ex.toString());
            throw new com.zeroc.Ice.PluginInitializationException(b.toString());
        }

        //
        // Create one lookup proxy per endpoint from the given proxy. We want to send a multicast
        // datagram on each endpoint.
        //
        com.zeroc.Ice.Endpoint[] single = new com.zeroc.Ice.Endpoint[1];
        for(com.zeroc.Ice.Endpoint endpt : lookup.ice_getEndpoints())
        {
            try
            {
                single[0] = endpt;
                LookupPrx l = (LookupPrx)lookup.ice_endpoints(single);
                l.ice_getConnection();
                _lookup.put(l, null);
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
            }
        }
        assert(!_lookup.isEmpty());
    }

    void setLookupReply(LookupReplyPrx lookupReply)
    {
        //
        // Use a lookup reply proxy whose adress matches the interface used to send multicast datagrams.
        //
        com.zeroc.Ice.Endpoint[] single = new com.zeroc.Ice.Endpoint[1];
        for(Map.Entry<LookupPrx, LookupReplyPrx> entry : _lookup.entrySet())
        {
            com.zeroc.Ice.UDPEndpointInfo info =
                (com.zeroc.Ice.UDPEndpointInfo)entry.getKey().ice_getEndpoints()[0].getInfo();
            if(!info.mcastInterface.isEmpty())
            {
                for(com.zeroc.Ice.Endpoint q : lookupReply.ice_getEndpoints())
                {
                    com.zeroc.Ice.EndpointInfo r = q.getInfo();
                    if(r instanceof com.zeroc.Ice.IPEndpointInfo &&
                       ((com.zeroc.Ice.IPEndpointInfo)r).host.equals(info.mcastInterface))
                    {
                        single[0] = q;
                        entry.setValue((LookupReplyPrx)lookupReply.ice_endpoints(single));
                    }
                }
            }

            if(entry.getValue() == null)
            {
                // Fallback: just use the given lookup reply proxy if no matching endpoint found.
                entry.setValue(lookupReply);
            }
        }
    }

    @Override
    public void findObjectById(String domainId, com.zeroc.Ice.Identity id, com.zeroc.IceDiscovery.LookupReplyPrx reply,
                               com.zeroc.Ice.Current c)
    {
        if(!domainId.equals(_domainId))
        {
            return; // Ignore.
        }

        com.zeroc.Ice.ObjectPrx proxy = _registry.findObject(id);
        if(proxy != null)
        {
            //
            // Reply to the mulicast request using the given proxy.
            //
            try
            {
               reply.foundObjectByIdAsync(id, proxy);
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
                // Ignore
            }
        }
    }

    @Override
    public void findAdapterById(String domainId, String adapterId, com.zeroc.IceDiscovery.LookupReplyPrx reply,
                                com.zeroc.Ice.Current c)
    {
        if(!domainId.equals(_domainId))
        {
            return; // Ignore.
        }

        LocatorRegistryI.FindAdapterResult r = _registry.findAdapter(adapterId);
        if(r.returnValue != null)
        {
            //
            // Reply to the multicast request using the given proxy.
            //
            try
            {
                reply.foundAdapterByIdAsync(adapterId, r.returnValue, r.isReplicaGroup);
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
                // Ignore
            }
        }
    }

    synchronized void findObject(CompletableFuture<com.zeroc.Ice.ObjectPrx> f, com.zeroc.Ice.Identity id)
    {
        ObjectRequest request = _objectRequests.get(id);
        if(request == null)
        {
            request = new ObjectRequest(id, _retryCount);
            _objectRequests.put(id, request);
        }

        if(request.addFuture(f))
        {
            try
            {
                for(Map.Entry<LookupPrx, LookupReplyPrx> entry : _lookup.entrySet())
                {
                    entry.getKey().findObjectByIdAsync(_domainId, id, entry.getValue());
                }
                request.scheduleTimer(_timeout);
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
                request.finished(null);
                _objectRequests.remove(id);
            }
        }
    }

    synchronized void findAdapter(CompletableFuture<com.zeroc.Ice.ObjectPrx> f, String adapterId)
    {
        AdapterRequest request = _adapterRequests.get(adapterId);
        if(request == null)
        {
            request = new AdapterRequest(adapterId, _retryCount);
            _adapterRequests.put(adapterId, request);
        }

        if(request.addFuture(f))
        {
            try
            {
                for(Map.Entry<LookupPrx, LookupReplyPrx> entry : _lookup.entrySet())
                {
                    entry.getKey().findAdapterByIdAsync(_domainId, adapterId, entry.getValue());
                }
                request.scheduleTimer(_timeout);
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
                request.finished(null);
                _adapterRequests.remove(adapterId);
            }
        }
    }

    synchronized void foundObject(com.zeroc.Ice.Identity id, com.zeroc.Ice.ObjectPrx proxy)
    {
        ObjectRequest request = _objectRequests.get(id);
        if(request == null)
        {
            return;
        }

        request.response(proxy);
        request.cancelTimer();
        _objectRequests.remove(id);
    }

    synchronized void foundAdapter(String adapterId, com.zeroc.Ice.ObjectPrx proxy, boolean isReplicaGroup)
    {
        AdapterRequest request = _adapterRequests.get(adapterId);
        if(request == null)
        {
            return;
        }

        if(request.response(proxy, isReplicaGroup))
        {
            request.cancelTimer();
            _adapterRequests.remove(adapterId);
        }
    }

    synchronized void objectRequestTimedOut(ObjectRequest request)
    {
        ObjectRequest r = _objectRequests.get(request.getId());
        if(r == null || request != r)
        {
            return;
        }

        if(request.retry())
        {
            try
            {
                for(Map.Entry<LookupPrx, LookupReplyPrx> entry : _lookup.entrySet())
                {
                    entry.getKey().findObjectByIdAsync(_domainId, request.getId(), entry.getValue());
                }
                request.scheduleTimer(_timeout);
                return;
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
            }
        }

        request.finished(null);
        _objectRequests.remove(request.getId());
    }

    synchronized void adapterRequestTimedOut(AdapterRequest request)
    {
        AdapterRequest r = _adapterRequests.get(request.getId());
        if(r == null || r != request)
        {
            return;
        }

        if(request.retry())
        {
            try
            {
                for(Map.Entry<LookupPrx, LookupReplyPrx> entry : _lookup.entrySet())
                {
                    entry.getKey().findAdapterByIdAsync(_domainId, request.getId(), entry.getValue());
                }
                request.scheduleTimer(_timeout);
                return;
            }
            catch(com.zeroc.Ice.LocalException ex)
            {
            }
        }

        request.finished(null);
        _adapterRequests.remove(request.getId());
    }

    private LocatorRegistryI _registry;
    private java.util.Map<LookupPrx, LookupReplyPrx> _lookup = new java.util.HashMap<>();
    private final int _timeout;
    private final int _retryCount;
    private final int _latencyMultiplier;
    private final String _domainId;

    private final java.util.concurrent.ScheduledExecutorService _timer;

    private Map<com.zeroc.Ice.Identity, ObjectRequest> _objectRequests = new HashMap<>();
    private Map<String, AdapterRequest> _adapterRequests = new HashMap<>();
}
