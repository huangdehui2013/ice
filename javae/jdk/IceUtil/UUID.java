// **********************************************************************
//
// Copyright (c) 2003-2005 ZeroC, Inc. All rights reserved.
//
// This copy of Ice-E is licensed to you under the terms described in the
// ICEE_LICENSE file included in this distribution.
//
// **********************************************************************

package IceUtil;

final public class UUID
{
    static public String
    create()
    {
	return new java.rmi.server.UID().toString();
    }
}
