// **********************************************************************
//
// Copyright (c) 2003-2005 ZeroC, Inc. All rights reserved.
//
// This copy of Ice-E is licensed to you under the terms described in the
// ICEE_LICENSE file included in this distribution.
//
// **********************************************************************

import Test.*;

public final class DI extends _DDisp
{
    public
    DI()
    {
    }

    public String
    callA(Ice.Current current)
    {
        return "A";
    }

    public String
    callB(Ice.Current current)
    {
        return "B";
    }

    public String
    callC(Ice.Current current)
    {
        return "C";
    }

    public String
    callD(Ice.Current current)
    {
        return "D";
    }
}
