// **********************************************************************
//
// Copyright (c) 2003-2016 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <Gen.h>
#include <Slice/Checksum.h>
#include <Slice/Util.h>
#include <IceUtil/Functional.h>
#include <IceUtil/Iterator.h>
#include <IceUtil/StringUtil.h>
#include <IceUtil/InputUtil.h>
#include <cstring>

#include <limits>

using namespace std;
using namespace Slice;
using namespace IceUtil;
using namespace IceUtilInternal;

namespace
{

string
sliceModeToIceMode(Operation::Mode opMode)
{
    string mode = "com.zeroc.Ice.OperationMode.";
    switch(opMode)
    {
    case Operation::Normal:
        mode = "null"; // shorthand for most common case
        break;
    case Operation::Nonmutating:
        mode += "Nonmutating";
        break;
    case Operation::Idempotent:
        mode += "Idempotent";
        break;
    default:
        assert(false);
        break;
    }
    return mode;
}

string
opFormatTypeToString(const OperationPtr& op)
{
    string format = "com.zeroc.Ice.FormatType.";
    switch(op->format())
    {
    case DefaultFormat:
        format = "null"; // shorthand for most common case
        break;
    case CompactFormat:
        format += "CompactFormat";
        break;
    case SlicedFormat:
        format += "SlicedFormat";
        break;
    default:
        assert(false);
        break;
    }
    return format;
}

string
getEscapedParamName(const OperationPtr& p, const string& name)
{
    ParamDeclList params = p->parameters();

    for(ParamDeclList::const_iterator i = params.begin(); i != params.end(); ++i)
    {
        if((*i)->name() == name)
        {
            return name + "_";
        }
    }
    return name;
}

string
getEscapedParamName(const DataMemberList& params, const string& name)
{
    for(DataMemberList::const_iterator i = params.begin(); i != params.end(); ++i)
    {
        if((*i)->name() == name)
        {
            return name + "_";
        }
    }
    return name;
}

bool
isDeprecated(const ContainedPtr& p1, const ContainedPtr& p2)
{
    string deprecateMetadata;
    return p1->findMetaData("deprecate", deprecateMetadata) ||
            (p2 != 0 && p2->findMetaData("deprecate", deprecateMetadata));
}

bool isValue(const TypePtr& type)
{
    BuiltinPtr b = BuiltinPtr::dynamicCast(type);
    ClassDeclPtr cl = ClassDeclPtr::dynamicCast(type);
    return (b && b->usesClasses()) || cl;
}


// Returns java.util.OptionalXXX.ofYYY depending on the type
string ofFactory(const TypePtr& type)
{
    const BuiltinPtr b = BuiltinPtr::dynamicCast(type);

    if(b)
    {
        if(b->kind() == Builtin::KindInt)
        {
            return "java.util.OptionalInt.of";
        }
        else if(b->kind() == Builtin::KindLong)
        {
            return "java.util.OptionalLong.of";
        }
        else if(b->kind() == Builtin::KindDouble)
        {
            return "java.util.OptionalDouble.of";
        }
        else if(b->kind() < Builtin::KindString)
        {
            return "java.util.Optional.of";
        }
    }

    return "java.util.Optional.ofNullable";
}

}

Slice::JavaVisitor::JavaVisitor(const string& dir) :
    JavaGenerator(dir)
{
}

Slice::JavaVisitor::~JavaVisitor()
{
}

string
Slice::JavaVisitor::getResultType(const OperationPtr& op, const string& package, bool object, bool dispatch)
{
    if(dispatch && op->hasMarshaledResult())
    {
        const ClassDefPtr c = ClassDefPtr::dynamicCast(op->container());
        assert(c);
        string abs;
        if(c->isInterface())
        {
            abs = getAbsolute(c, package);
        }
        else
        {
            abs = getAbsolute(c, package, "_", "Disp");
        }
        string name = op->name();
        name[0] = toupper(static_cast<unsigned char>(name[0]));
        return abs + "." + name + "MarshaledResult";
    }
    else if(op->returnsMultipleValues())
    {
        const ContainedPtr c = ContainedPtr::dynamicCast(op->container());
        assert(c);
        const string abs = getAbsolute(c, package);
        string name = op->name();
        name[0] = toupper(static_cast<unsigned char>(name[0]));
        return abs + "." + name + "Result";
    }
    else
    {
        TypePtr type = op->returnType();
        bool optional = op->returnIsOptional();
        if(!type)
        {
            const ParamDeclList outParams = op->outParameters();
            if(!outParams.empty())
            {
                assert(outParams.size() == 1);
                type = outParams.front()->type();
                optional = outParams.front()->optional();
            }
        }
        if(type)
        {
            ClassDefPtr cl = ClassDefPtr::dynamicCast(op->container());
            assert(cl);
            if(optional)
            {
                return typeToString(type, TypeModeReturn, package, op->getMetaData(), true, true, cl->isLocal());
            }
            else if(object)
            {
                return typeToObjectString(type, TypeModeReturn, package, op->getMetaData(), true, cl->isLocal());
            }
            else
            {
                return typeToString(type, TypeModeReturn, package, op->getMetaData(), true, false, cl->isLocal());
            }
        }
        else
        {
            return object ? "Void" : "void";
        }
    }
}

void
Slice::JavaVisitor::writeResultType(Output& out, const OperationPtr& op, const string& package, const DocCommentPtr& dc)
{
    string opName = op->name();
    opName[0] = toupper(static_cast<unsigned char>(opName[0]));

    out << sp << nl << "public static class " << opName << "Result";
    out << sb;

    //
    // Make sure none of the out parameters are named "returnValue".
    //
    string retval = "returnValue";
    const ParamDeclList outParams = op->outParameters();
    for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
    {
        if((*p)->name() == "returnValue")
        {
            retval = "_returnValue";
            break;
        }
    }

    const TypePtr ret = op->returnType();
    const ClassDefPtr cl = ClassDefPtr::dynamicCast(op->container());
    assert(cl);

    //
    // Default constructor.
    //
    out << nl << "public " << opName << "Result()";
    out << sb;
    out << eb;

    //
    // One-shot constructor.
    //

    bool needMandatoryOnly = false;
    bool generateMandatoryOnly = false;

    do
    {
        out << sp;

        if(needMandatoryOnly)
        {
            generateMandatoryOnly = true;
            needMandatoryOnly = false;
        }

        if(dc)
        {
            //
            // Emit a doc comment for the constructor if necessary.
            //
            out << nl << "/**";
            out << nl << " * This constructor makes shallow copies of the results for operation " << opName;
            if(generateMandatoryOnly)
            {
                out << " (overload without Optional parameters).";
            }
            else
            {
                out << '.';
            }

            if(ret && !dc->returns.empty())
            {
                out << nl << " * @param " << retval << ' ';
                writeDocCommentLines(out, dc->returns);
            }
            for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
            {
                const string name = (*p)->name();
                map<string, string>::const_iterator q = dc->params.find(name);
                if(q != dc->params.end() && !q->second.empty())
                {
                    out << nl << " * @param " << fixKwd(q->first) << ' ';
                    writeDocCommentLines(out, q->second);
                }
            }
            out << nl << " **/";
        }

        out << nl << "public " << opName << "Result" << spar;

        if(ret)
        {
            out << (typeToString(ret, TypeModeIn, package, op->getMetaData(), true, !generateMandatoryOnly && op->returnIsOptional(),
                                 cl->isLocal()) + " " + retval);
            needMandatoryOnly = !generateMandatoryOnly && op->returnIsOptional();
        }
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            out << (typeToString((*p)->type(), TypeModeIn, package, (*p)->getMetaData(), true,
                                 !generateMandatoryOnly && (*p)->optional(), cl->isLocal()) + " " + fixKwd((*p)->name()));
            if(!generateMandatoryOnly)
            {
                needMandatoryOnly = needMandatoryOnly || (*p)->optional();
            }
        }
        out << epar;
        out << sb;
        if(ret)
        {
            out << nl << "this." << retval << " = ";
            if(op->returnIsOptional() && generateMandatoryOnly)
            {
                out << ofFactory(ret) << "(" << retval << ");";
            }
            else
            {
                out << retval << ';';
            }

        }
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            const string name = fixKwd((*p)->name());
            out << nl << "this." << name << " = ";
            if((*p)->optional() && generateMandatoryOnly)
            {
                out << ofFactory((*p)->type()) << "(" << name << ");";
            }
            else
            {
                out << name << ';';
            }
        }
        out << eb;
    } while(needMandatoryOnly);

    //
    // Members.
    //
    out << sp;
    if(ret)
    {
        if(dc && !dc->returns.empty())
        {
            out << nl << "/**";
            out << nl << " * ";
            writeDocCommentLines(out, dc->returns);
            out << nl << " **/";
        }
        out << nl << "public " << typeToString(ret, TypeModeIn, package, op->getMetaData(), true,
                                               op->returnIsOptional(), cl->isLocal())
            << ' ' << retval << ';';
    }

    for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
    {
        if(dc)
        {
            const string name = (*p)->name();
            map<string, string>::const_iterator q = dc->params.find(name);
            if(q != dc->params.end() && !q->second.empty())
            {
                out << nl << "/**";
                out << nl << " * ";
                writeDocCommentLines(out, q->second);
                out << nl << " **/";
            }
        }
        out << nl << "public " << typeToString((*p)->type(), TypeModeIn, package, (*p)->getMetaData(), true,
                                               (*p)->optional(), cl->isLocal())
            << ' ' << fixKwd((*p)->name()) << ';';
    }

    if(!cl->isLocal())
    {
        ParamDeclList required, optional;
        op->outParameters(required, optional);

        out << sp << nl << "public void write(com.zeroc.Ice.OutputStream ostr)";
        out << sb;

        int iter = 0;
        for(ParamDeclList::const_iterator pli = required.begin(); pli != required.end(); ++pli)
        {
            const string paramName = fixKwd((*pli)->name());
            writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalNone, false, 0, "this." + paramName, true, iter, "",
                                      (*pli)->getMetaData());
        }

        if(ret && !op->returnIsOptional())
        {
            writeMarshalUnmarshalCode(out, package, ret, OptionalNone, false, 0, retval, true, iter, "", op->getMetaData());
        }

        //
        // Handle optional parameters.
        //
        bool checkReturnType = op->returnIsOptional();

        for(ParamDeclList::const_iterator pli = optional.begin(); pli != optional.end(); ++pli)
        {
            if(checkReturnType && op->returnTag() < (*pli)->tag())
            {
                writeMarshalUnmarshalCode(out, package, ret, OptionalReturnParam, true, op->returnTag(), retval, true,
                                          iter, "", op->getMetaData());
                checkReturnType = false;
            }

            const string paramName = fixKwd((*pli)->name());
            writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalOutParam, true, (*pli)->tag(), "this." + paramName,
                                      true, iter, "", (*pli)->getMetaData());
        }

        if(checkReturnType)
        {
            writeMarshalUnmarshalCode(out, package, ret, OptionalReturnParam, true, op->returnTag(), retval, true, iter,
                                      "", op->getMetaData());
        }

        out << eb;

        out << sp << nl << "public void read(com.zeroc.Ice.InputStream istr)";
        out << sb;

        iter = 0;
        for(ParamDeclList::const_iterator pli = required.begin(); pli != required.end(); ++pli)
        {
            const string paramName = fixKwd((*pli)->name());
            const string patchParams = getPatcher((*pli)->type(), package, paramName, false);
            writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalNone, false, 0, "this." + paramName, false, iter,
                                      "", (*pli)->getMetaData(), patchParams);
        }

        if(ret && !op->returnIsOptional())
        {
            const string patchParams = getPatcher(ret, package, retval, false);
            writeMarshalUnmarshalCode(out, package, ret, OptionalNone, false, 0, retval, false, iter, "", op->getMetaData(),
                                      patchParams);
        }

        //
        // Handle optional parameters.
        //
        checkReturnType = op->returnIsOptional();

        for(ParamDeclList::const_iterator pli = optional.begin(); pli != optional.end(); ++pli)
        {
            if(checkReturnType && op->returnTag() < (*pli)->tag())
            {
                const string patchParams = getPatcher(ret, package, retval, true);
                writeMarshalUnmarshalCode(out, package, ret, OptionalReturnParam, true, op->returnTag(), retval, false,
                                          iter, "", op->getMetaData(), patchParams);
                checkReturnType = false;
            }

            const string paramName = fixKwd((*pli)->name());
            const string patchParams = getPatcher((*pli)->type(), package, paramName, true);
            writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalOutParam, true, (*pli)->tag(), "this." + paramName,
                                      false, iter, "", (*pli)->getMetaData(), patchParams);
        }

        if(checkReturnType)
        {
            const string patchParams = getPatcher(ret, package, retval, true);
            writeMarshalUnmarshalCode(out, package, ret, OptionalReturnParam, true, op->returnTag(), retval, false,
                                      iter, "", op->getMetaData(), patchParams);
        }

        out << eb;
    }

    out << eb;
}

void
Slice::JavaVisitor::writeMarshaledResultType(Output& out, const OperationPtr& op, const string& package,
                                             const DocCommentPtr& dc)
{
    string opName = op->name();
    opName[0] = toupper(static_cast<unsigned char>(opName[0]));

    out << sp << nl << "public static class " << opName << "MarshaledResult implements com.zeroc.Ice.MarshaledResult";
    out << sb;

    const TypePtr ret = op->returnType();
    const ClassDefPtr cl = ClassDefPtr::dynamicCast(op->container());
    assert(cl);
    const ParamDeclList outParams = op->outParameters();
    const string retval = getEscapedParamName(op, "returnValue");
    const string currentParamName = getEscapedParamName(op, "current");
    const string currentParam = "com.zeroc.Ice.Current " + currentParamName;

    out << sp;

    //
    // Emit a doc comment for the constructor if necessary.
    //
    if(dc)
    {
        out << nl << "/**";
        out << nl << " * This constructor marshals the results of operation " << opName << " immediately.";

        if(ret && !dc->returns.empty())
        {
            out << nl << " * @param " << retval << ' ';
            writeDocCommentLines(out, dc->returns);
        }
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            const string name = (*p)->name();
            map<string, string>::const_iterator q = dc->params.find(name);
            if(q != dc->params.end() && !q->second.empty())
            {
                out << nl << " * @param " << fixKwd(q->first) << ' ';
                writeDocCommentLines(out, q->second);
            }
        }
        out << nl << " * @param " << currentParamName << " The Current object for the invocation.";
        out << nl << " **/";
    }

    bool hasOpt = false;
    out << nl << "public " << opName << "MarshaledResult" << spar;
    if(ret)
    {
        out << (typeToString(ret, TypeModeIn, package, op->getMetaData(), true, op->returnIsOptional(), cl->isLocal())
            + " " + retval);
        hasOpt = op->returnIsOptional();
    }
    for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
    {
        out << (typeToString((*p)->type(), TypeModeIn, package, (*p)->getMetaData(), true, (*p)->optional(),
                             cl->isLocal()) + " " + fixKwd((*p)->name()));

        hasOpt = hasOpt || (*p)->optional();
    }
    out << currentParam << epar;
    out << sb;
    out << nl << "_ostr = com.zeroc.IceInternal.Incoming.createResponseOutputStream(" << currentParamName << ");";
    out << nl << "_ostr.startEncapsulation(" << currentParamName << ".encoding, " << opFormatTypeToString(op) << ");";

    ParamDeclList required, optional;
    op->outParameters(required, optional);
    int iter = 0;
    for(ParamDeclList::const_iterator pli = required.begin(); pli != required.end(); ++pli)
    {
        const string paramName = fixKwd((*pli)->name());
        writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalNone, false, 0, paramName, true, iter,
                                  "_ostr", (*pli)->getMetaData());
    }

    if(ret && !op->returnIsOptional())
    {
        writeMarshalUnmarshalCode(out, package, ret, OptionalNone, false, 0, retval, true, iter, "_ostr",
                                  op->getMetaData());
    }

    //
    // Handle optional parameters.
    //
    bool checkReturnType = op->returnIsOptional();

    for(ParamDeclList::const_iterator pli = optional.begin(); pli != optional.end(); ++pli)
    {
        if(checkReturnType && op->returnTag() < (*pli)->tag())
        {
            writeMarshalUnmarshalCode(out, package, ret, OptionalReturnParam, true, op->returnTag(), retval, true,
                                      iter, "_ostr", op->getMetaData());
            checkReturnType = false;
        }

        const string paramName = fixKwd((*pli)->name());
        writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalOutParam, true, (*pli)->tag(), paramName,
                                  true, iter, "_ostr", (*pli)->getMetaData());
    }

    if(checkReturnType)
    {
        writeMarshalUnmarshalCode(out, package, ret, OptionalReturnParam, true, op->returnTag(), retval, true, iter,
                                  "_ostr", op->getMetaData());
    }

    if(op->returnsClasses(false))
    {
        out << nl << "_ostr.writePendingValues();";
    }

    out << nl << "_ostr.endEncapsulation();";

    out << eb;

    if(hasOpt)
    {
        out << sp;

        //
        // Emit a doc comment for the constructor if necessary.
        //
        if(dc)
        {
            out << nl << "/**";
            out << nl << " * This constructor marshals the results of operation " << opName
                << " immediately (overload without Optional parameters).";

            if(ret && !dc->returns.empty())
            {
                out << nl << " * @param " << retval << ' ';
                writeDocCommentLines(out, dc->returns);
            }
            for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
            {
                const string name = (*p)->name();
                map<string, string>::const_iterator q = dc->params.find(name);
                if(q != dc->params.end() && !q->second.empty())
                {
                    out << nl << " * @param " << fixKwd(q->first) << ' ';
                    writeDocCommentLines(out, q->second);
                }
            }
            out << nl << " * @param " << currentParamName << " The Current object for the invocation.";
            out << nl << " **/";
        }

        out << nl << "public " << opName << "MarshaledResult" << spar;
        if(ret)
        {
            out << (typeToString(ret, TypeModeIn, package, op->getMetaData(), true, false, cl->isLocal())
                    + " " + retval);
        }
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            out << (typeToString((*p)->type(), TypeModeIn, package, (*p)->getMetaData(), true, false,
                                 cl->isLocal()) + " " + fixKwd((*p)->name()));
        }

        out << currentParam << epar;
        out << sb;
        out << nl << "this" << spar;
        if(ret)
        {
            if(op->returnIsOptional())
            {
                out << ofFactory(ret) + "(" + retval + ")";
            }
            else
            {
                out << retval;
            }
        }
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            if((*p)->optional())
            {
                out << ofFactory((*p)->type()) + "(" + fixKwd((*p)->name()) + ")";
            }
            else
            {
                out << fixKwd((*p)->name());
            }
        }

        out << currentParamName << epar << ';';
        out << eb;
    }

    out << sp;
    out << nl << "@Override"
        << nl << "public com.zeroc.Ice.OutputStream getOutputStream()"
        << sb
        << nl << "return _ostr;"
        << eb;

    out << sp;
    out << nl << "private com.zeroc.Ice.OutputStream _ostr;";
    out << eb;
}

void
Slice::JavaVisitor::allocatePatcher(Output& out, const TypePtr& type, const string& package, const string& name)
{
    BuiltinPtr b = BuiltinPtr::dynamicCast(type);
    ClassDeclPtr cl = ClassDeclPtr::dynamicCast(type);
    assert((b && b->usesClasses()) || cl);

    string clsName;
    if(b || cl->isInterface())
    {
        clsName = "com.zeroc.Ice.Value";
    }
    else
    {
        clsName = getAbsolute(cl, package);
    }

    out << nl << "com.zeroc.IceInternal.Patcher<" << clsName << "> " << name << " = new com.zeroc.IceInternal.Patcher<"
        << clsName << ">(" << clsName << ".class, " << clsName << ".ice_staticId());";
}

string
Slice::JavaVisitor::getPatcher(const TypePtr& type, const string& package, const string& dest, bool optionalMapping)
{
    BuiltinPtr b = BuiltinPtr::dynamicCast(type);
    ClassDeclPtr cl = ClassDeclPtr::dynamicCast(type);
    ostringstream ostr;
    if((b && b->usesClasses()) || cl)
    {
        string clsName;
        if(b || cl->isInterface())
        {
            clsName = "com.zeroc.Ice.Value";
        }
        else
        {
            clsName = getAbsolute(cl, package);
        }

        ostr << "new com.zeroc.IceInternal.Patcher<" << clsName << ">(" << clsName << ".class, "
             << clsName << ".ice_staticId(), " << "v -> ";
        if(optionalMapping)
        {
            ostr << dest << " = java.util.Optional.ofNullable(v)";
        }
        else
        {
            ostr << dest << " = v";
        }
        ostr << ')';
    }
    return ostr.str();
}

string
Slice::JavaVisitor::getFutureType(const OperationPtr& op, const string& package)
{
    if(op->returnType() || op->outParameters().size() > 0)
    {
        return "java.util.concurrent.CompletableFuture<" + getResultType(op, package, true, false) + ">";
    }
    else
    {
        return "java.util.concurrent.CompletableFuture<Void>";
    }
}

string
Slice::JavaVisitor::getFutureImplType(const OperationPtr& op, const string& package)
{
    if(op->returnType() || op->outParameters().size() > 0)
    {
        return "com.zeroc.IceInternal.OutgoingAsync<" + getResultType(op, package, true, false) + ">";
    }
    else
    {
        return "com.zeroc.IceInternal.OutgoingAsync<Void>";
    }
}

vector<string>
Slice::JavaVisitor::getParams(const OperationPtr& op, const string& package)
{
    vector<string> params;

    const ClassDefPtr cl = ClassDefPtr::dynamicCast(op->container());
    assert(cl);

    const ParamDeclList paramList = op->inParameters();
    for(ParamDeclList::const_iterator q = paramList.begin(); q != paramList.end(); ++q)
    {
        const string type = typeToString((*q)->type(), TypeModeIn, package, (*q)->getMetaData(), true,
                                         (*q)->optional(), cl->isLocal());
        params.push_back(type + ' ' + fixKwd((*q)->name()));
    }

    return params;
}

vector<string>
Slice::JavaVisitor::getParamsProxy(const OperationPtr& op, const string& package, bool optionalMapping, bool internal)
{
    vector<string> params;

    ParamDeclList inParams = op->inParameters();
    for(ParamDeclList::const_iterator q = inParams.begin(); q != inParams.end(); ++q)
    {
        const string typeString = typeToString((*q)->type(), TypeModeIn, package, (*q)->getMetaData(), true,
                                               optionalMapping && (*q)->optional());
        params.push_back(typeString + ' ' + (internal ? "iceP_" + (*q)->name() : fixKwd((*q)->name())));
    }

    return params;
}

vector<string>
Slice::JavaVisitor::getArgs(const OperationPtr& op)
{
    vector<string> args;

    ParamDeclList paramList = op->parameters();
    for(ParamDeclList::const_iterator q = paramList.begin(); q != paramList.end(); ++q)
    {
        args.push_back(fixKwd((*q)->name()));
    }

    return args;
}

vector<string>
Slice::JavaVisitor::getInArgs(const OperationPtr& op, bool internal)
{
    vector<string> args;

    ParamDeclList paramList = op->inParameters();
    for(ParamDeclList::const_iterator q = paramList.begin(); q != paramList.end(); ++q)
    {
        string s = internal ? "iceP_" + (*q)->name() : fixKwd((*q)->name());
        args.push_back(s);
    }

    return args;
}

void
Slice::JavaVisitor::writeMarshalProxyParams(Output& out, const string& package, const OperationPtr& op,
                                            bool optionalMapping)
{
    int iter = 0;
    ParamDeclList required, optional;
    op->inParameters(required, optional);
    for(ParamDeclList::const_iterator pli = required.begin(); pli != required.end(); ++pli)
    {
        string paramName = "iceP_" + (*pli)->name();
        writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalNone, false, 0, paramName, true,
                                  iter, "", (*pli)->getMetaData());
    }

    //
    // Handle optional parameters.
    //
    for(ParamDeclList::const_iterator pli = optional.begin(); pli != optional.end(); ++pli)
    {
        writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalInParam, optionalMapping,
                                  (*pli)->tag(), "iceP_" + (*pli)->name(), true, iter, "", (*pli)->getMetaData());
    }

    if(op->sendsClasses(false))
    {
        out << nl << "ostr.writePendingValues();";
    }
}

void
Slice::JavaVisitor::writeUnmarshalProxyResults(Output& out, const string& package, const OperationPtr& op)
{
    const ParamDeclList outParams = op->outParameters();
    const TypePtr ret = op->returnType();
    const string name = "ret";

    if(op->returnsMultipleValues())
    {
        string resultType = getResultType(op, package, false, false);
        out << nl << resultType << ' ' << name << " = new " << resultType << "();";
        out << nl << name << ".read(istr);";
        if(op->returnsClasses(false))
        {
            out << nl << "istr.readPendingValues();";
        }
        out << nl << "return " << name << ';';
    }
    else
    {
        string resultType = getResultType(op, package, false, false);

        bool optional;
        TypePtr type;
        int tag;
        StringList metaData;
        if(ret)
        {
            type = ret;
            optional = op->returnIsOptional();
            tag = op->returnTag();
            metaData = op->getMetaData();
        }
        else
        {
            assert(outParams.size() == 1);
            optional = outParams.front()->optional();
            type = outParams.front()->type();
            tag = outParams.front()->tag();
            metaData = outParams.front()->getMetaData();
        }

        const bool val = isValue(type);

        int iter = 0;

        if(optional)
        {
            if(val)
            {
                allocatePatcher(out, type, package, name);
            }
            else
            {
                out << nl << resultType << ' ' << name << ';';
            }
            writeMarshalUnmarshalCode(out, package, type, ret ? OptionalReturnParam : OptionalOutParam, true,
                                      tag, name, false, iter, "", metaData, name);
        }
        else
        {
            if(val)
            {
                allocatePatcher(out, type, package, name);
            }
            else if(StructPtr::dynamicCast(type))
            {
                out << nl << resultType << ' ' << name << " = null;";
            }
            else
            {
                out << nl << resultType << ' ' << name << ';';
            }
            writeMarshalUnmarshalCode(out, package, type, OptionalNone, false, 0, name, false, iter, "", metaData, name);
        }

        if(op->returnsClasses(false))
        {
            out << nl << "istr.readPendingValues();";
        }

        if(optional && val)
        {
            out << nl << "return java.util.Optional.ofNullable(" << name << ".value);";
        }
        else if(val)
        {
            out << nl << "return " << name << ".value;";
        }
        else
        {
            out << nl << "return " << name << ';';
        }
    }
}

void
Slice::JavaVisitor::writeMarshalServantResults(Output& out, const string& package, const OperationPtr& op,
                                               const string& param)
{
    if(op->returnsMultipleValues())
    {
        out << nl << param << ".write(ostr);";
    }
    else
    {
        const ParamDeclList params = op->outParameters();
        bool optional;
        OptionalMode mode;
        TypePtr type;
        int tag;
        StringList metaData;
        if(op->returnType())
        {
            type = op->returnType();
            optional = op->returnIsOptional();
            mode = optional ? OptionalReturnParam : OptionalNone;
            tag = op->returnTag();
            metaData = op->getMetaData();
        }
        else
        {
            assert(params.size() == 1);
            optional = params.front()->optional();
            mode = optional ? OptionalOutParam : OptionalNone;
            type = params.front()->type();
            tag = params.front()->tag();
            metaData = params.front()->getMetaData();
        }

        int iter = 0;
        writeMarshalUnmarshalCode(out, package, type, mode, true, tag, param, true, iter, "", metaData);
    }

    if(op->returnsClasses(false))
    {
        out << nl << "ostr.writePendingValues();";
    }
}

void
Slice::JavaVisitor::writeThrowsClause(const string& package, const ExceptionList& throws, const OperationPtr& op)
{
    Output& out = output();

    if(op && (op->hasMetaData("java:UserException") || op->hasMetaData("UserException")))
    {
        out.inc();
        out << nl << "throws com.zeroc.Ice.UserException";
        out.dec();
    }
    else if(throws.size() > 0)
    {
        out.inc();
        out << nl << "throws ";
        out.useCurrentPosAsIndent();
        int count = 0;
        for(ExceptionList::const_iterator r = throws.begin(); r != throws.end(); ++r)
        {
            if(count > 0)
            {
                out << "," << nl;
            }
            out << getAbsolute(*r, package);
            count++;
        }
        out.restoreIndent();
        out.dec();
    }
}

void
Slice::JavaVisitor::writeMarshalDataMember(Output& out, const string& package, const DataMemberPtr& member, int& iter, bool forStruct)
{
    if(member->optional())
    {
        assert(!forStruct);
        out << nl << "if(_" << member->name() << " && ostr_.writeOptional(" << member->tag() << ", "
            << getOptionalFormat(member->type()) << "))";
        out << sb;
        writeMarshalUnmarshalCode(out, package, member->type(), OptionalMember, false, 0, fixKwd(member->name()), true,
                                  iter, "ostr_", member->getMetaData());
        out << eb;
    }
    else
    {
        string stream = forStruct ? "" : "ostr_";
        string memberName = fixKwd(member->name());
        if(forStruct)
        {
            memberName = "this." + memberName;
        }

        writeMarshalUnmarshalCode(out, package, member->type(), OptionalNone, false, 0, memberName,
                                  true, iter, stream, member->getMetaData());
    }
}

void
Slice::JavaVisitor::writeUnmarshalDataMember(Output& out, const string& package, const DataMemberPtr& member, int& iter, bool forStruct)
{
    // TBD: Handle passing interface-by-value

    const string patchParams = getPatcher(member->type(), package, fixKwd(member->name()), false);

    if(member->optional())
    {
        assert(!forStruct);
        out << nl << "if(_" << member->name() << " = istr_.readOptional(" << member->tag() << ", "
            << getOptionalFormat(member->type()) << "))";
        out << sb;
        writeMarshalUnmarshalCode(out, package, member->type(), OptionalMember, false, 0, fixKwd(member->name()), false,
                                  iter, "istr_", member->getMetaData(), patchParams);
        out << eb;
    }
    else
    {
        string stream = forStruct ? "" : "istr_";
        string memberName = fixKwd(member->name());
        if(forStruct)
        {
            memberName = "this." + memberName;
        }

        writeMarshalUnmarshalCode(out, package, member->type(), OptionalNone, false, 0, memberName, false,
                                  iter, stream, member->getMetaData(), patchParams);
    }
}

void
Slice::JavaVisitor::writeDispatch(Output& out, const ClassDefPtr& p)
{
    const string name = fixKwd(p->name());
    const string package = getPackage(p);
    const string scoped = p->scoped();
    const ClassList bases = p->bases();
    ClassDefPtr base;
    if(!bases.empty() && !bases.front()->isInterface())
    {
        base = bases.front();
    }
    const OperationList ops = p->operations();

    for(OperationList::const_iterator r = ops.begin(); r != ops.end(); ++r)
    {
        OperationPtr op = *r;

        DocCommentPtr dc = parseDocComment(op);

        //
        // The "MarshaledResult" type is generated in the servant interface.
        //
        if(!p->isInterface() && op->hasMarshaledResult())
        {
            writeMarshaledResultType(out, op, package, dc);
        }

        vector<string> params = getParams(op, package);
        const string currentParam = "com.zeroc.Ice.Current " + getEscapedParamName(op, "current");

        const bool amd = p->hasMetaData("amd") || op->hasMetaData("amd");

        ExceptionList throws = op->throws();
        throws.sort();
        throws.unique();

        out << sp;
        writeServantDocComment(out, op, package, dc, amd);
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }

        if(amd)
        {
            out << nl << "java.util.concurrent.CompletionStage<" << getResultType(op, package, true, true) << "> "
                << op->name() << "Async" << spar << params << currentParam << epar;
            writeThrowsClause(package, throws, op);
            out << ';';
        }
        else
        {
            out << nl << getResultType(op, package, false, true) << ' ' << fixKwd(op->name()) << spar << params
                << currentParam << epar;
            writeThrowsClause(package, throws, op);
            out << ';';
        }
    }

    ClassList allBases = p->allBases();
    StringList ids;
    transform(allBases.begin(), allBases.end(), back_inserter(ids), constMemFun(&Contained::scoped));
    StringList other;
    other.push_back(scoped);
    other.push_back("::Ice::Object");
    other.sort();
    ids.merge(other);
    ids.unique();
#ifndef NDEBUG
    StringList::const_iterator scopedIter = find(ids.begin(), ids.end(), scoped);
    assert(scopedIter != ids.end());
#endif

    out << sp << nl << "static final String[] _iceIds =";
    out << sb;

    for(StringList::const_iterator q = ids.begin(); q != ids.end();)
    {
        out << nl << '"' << *q << '"';
        if(++q != ids.end())
        {
            out << ',';
        }
    }
    out << eb << ';';

    out << sp << nl << "@Override" << nl << "default String[] ice_ids(com.zeroc.Ice.Current current)";
    out << sb;
    out << nl << "return _iceIds;";
    out << eb;

    out << sp << nl << "@Override" << nl << "default String ice_id(com.zeroc.Ice.Current current)";
    out << sb;
    out << nl << "return ice_staticId();";
    out << eb;

    out << sp << nl;
    out << "static String ice_staticId()";
    out << sb;
    if(p->isInterface())
    {
        out << nl << "return \"" << p->scoped() << "\";";
    }
    else
    {
        out << nl << "return " << fixKwd(p->name()) << ".ice_staticId();";
    }
    out << eb;

    //
    // Dispatch methods. We only generate methods for operations
    // defined in this ClassDef, because we reuse existing methods
    // for inherited operations.
    //
    for(OperationList::const_iterator r = ops.begin(); r != ops.end(); ++r)
    {
        OperationPtr op = *r;
        StringList opMetaData = op->getMetaData();

        DocCommentPtr dc = parseDocComment(op);

        string opName = op->name();
        out << sp;
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "static java.util.concurrent.CompletionStage<com.zeroc.Ice.OutputStream> _iceD_" << opName << '(';
        if(p->isInterface())
        {
            out << name;
        }
        else
        {
            out << '_' << p->name() << "Disp";
        }
        out << " obj, final com.zeroc.IceInternal.Incoming inS, com.zeroc.Ice.Current current)";
        if(!op->throws().empty() || op->hasMetaData("java:UserException") || op->hasMetaData("UserException"))
        {
            out.inc();
            out << nl << "throws com.zeroc.Ice.UserException";
            out.dec();
        }
        out << sb;

        const bool amd = p->hasMetaData("amd") || op->hasMetaData("amd");

        const TypePtr ret = op->returnType();

        const ParamDeclList inParams = op->inParameters();
        const ParamDeclList outParams = op->outParameters();

        out << nl << "com.zeroc.Ice.Object._iceCheckMode(" << sliceModeToIceMode(op->mode()) << ", current.mode);";

        if(!inParams.empty())
        {
            ParamDeclList values;

            //
            // Declare 'in' parameters.
            //
            out << nl << "com.zeroc.Ice.InputStream istr = inS.startReadParams();";
            for(ParamDeclList::const_iterator pli = inParams.begin(); pli != inParams.end(); ++pli)
            {
                const TypePtr paramType = (*pli)->type();
                if(isValue(paramType))
                {
                    allocatePatcher(out, paramType, package, "icePP_" + (*pli)->name());
                    values.push_back(*pli);
                }
                else
                {
                    const string paramName = "iceP_" + (*pli)->name();
                    const string typeS = typeToString(paramType, TypeModeIn, package, (*pli)->getMetaData(), true,
                                                      (*pli)->optional());
                    if((*pli)->optional())
                    {
                        out << nl << typeS << ' ' << paramName << ';';
                    }
                    else
                    {
                        if(StructPtr::dynamicCast(paramType))
                        {
                            out << nl << typeS << ' ' << paramName << " = null;";
                        }
                        else
                        {
                            out << nl << typeS << ' ' << paramName << ';';
                        }
                    }
                }
            }

            //
            // Unmarshal 'in' parameters.
            //
            ParamDeclList required, optional;
            op->inParameters(required, optional);
            int iter = 0;
            for(ParamDeclList::const_iterator pli = required.begin(); pli != required.end(); ++pli)
            {
                const string paramName = isValue((*pli)->type()) ? ("icePP_" + (*pli)->name()) : "iceP_" + (*pli)->name();
                writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalNone, false, 0, paramName, false,
                                          iter, "", (*pli)->getMetaData(), paramName);
            }
            for(ParamDeclList::const_iterator pli = optional.begin(); pli != optional.end(); ++pli)
            {
                const string paramName = isValue((*pli)->type()) ? ("icePP_" + (*pli)->name()) : "iceP_" + (*pli)->name();
                writeMarshalUnmarshalCode(out, package, (*pli)->type(), OptionalInParam, true, (*pli)->tag(),
                                          paramName, false, iter, "", (*pli)->getMetaData(), paramName);
            }
            if(op->sendsClasses(false))
            {
                out << nl << "istr.readPendingValues();";
            }
            out << nl << "inS.endReadParams();";

            for(ParamDeclList::const_iterator pli = values.begin(); pli != values.end(); ++pli)
            {
                const TypePtr paramType = (*pli)->type();
                const string paramName = "iceP_" + (*pli)->name();
                const string typeS = typeToString(paramType, TypeModeIn, package, (*pli)->getMetaData(), true,
                                                  (*pli)->optional());
                if((*pli)->optional())
                {
                    out << nl << typeS << ' ' << paramName << " = java.util.Optional.ofNullable(icePP_" << (*pli)->name()
                        << ".value);";
                }
                else
                {
                    out << nl << typeS << ' ' << paramName << " = icePP_" << (*pli)->name() << ".value;";
                }
            }
        }
        else
        {
            out << nl << "inS.readEmptyParams();";
        }

        if(op->format() != DefaultFormat)
        {
            out << nl << "inS.setFormat(" << opFormatTypeToString(op) << ");";
        }

        if(amd)
        {
            if(op->hasMarshaledResult())
            {
                out << nl << "return inS.setMarshaledResultFuture(obj." << opName << "Async" << spar
                    << getInArgs(op, true) << "current" << epar << ");";
            }
            else
            {
                out << nl << "return inS.setResultFuture(obj." << opName << "Async" << spar << getInArgs(op, true)
                    << "current" << epar;
                if(ret || !outParams.empty())
                {
                    out << ", (ostr, ret) ->";
                    out.inc();
                    out << sb;
                    writeMarshalServantResults(out, package, op, "ret");
                    out << eb;
                    out.dec();
                }
                out << ");";
            }
        }
        else
        {
            //
            // Call on the servant.
            //
            out << nl;
            if(ret || !outParams.empty())
            {
                out << getResultType(op, package, false, true) << " ret = ";
            }
            out << "obj." << fixKwd(opName) << spar << getInArgs(op, true) << "current" << epar << ';';

            //
            // Marshal 'out' parameters and return value.
            //
            if(op->hasMarshaledResult())
            {
                out << nl << "return inS.setMarshaledResult(ret);";
            }
            else if(ret || !outParams.empty())
            {
                out << nl << "com.zeroc.Ice.OutputStream ostr = inS.startWriteParams();";
                writeMarshalServantResults(out, package, op, "ret");
                out << nl << "inS.endWriteParams(ostr);";
                out << nl << "return inS.setResult(ostr);";
            }
            else
            {
                out << nl << "return inS.setResult(inS.writeEmptyParams());";
            }
        }

        out << eb;
    }

    OperationList allOps = p->allOperations();
    if(!allOps.empty())
    {
        StringList allOpNames;
        transform(allOps.begin(), allOps.end(), back_inserter(allOpNames), constMemFun(&Contained::name));
        allOpNames.push_back("ice_id");
        allOpNames.push_back("ice_ids");
        allOpNames.push_back("ice_isA");
        allOpNames.push_back("ice_ping");
        allOpNames.sort();
        allOpNames.unique();

        out << sp << nl << "final static String[] _iceOps =";
        out << sb;
        for(StringList::const_iterator q = allOpNames.begin(); q != allOpNames.end();)
        {
            out << nl << '"' << *q << '"';
            if(++q != allOpNames.end())
            {
                out << ',';
            }
        }
        out << eb << ';';

        out << sp;
        for(OperationList::iterator r = allOps.begin(); r != allOps.end(); ++r)
        {
            //
            // Suppress deprecation warnings if this method dispatches to a deprecated operation.
            //
            OperationPtr op = *r;
            ContainerPtr container = op->container();
            ClassDefPtr cl = ClassDefPtr::dynamicCast(container);
            assert(cl);
            if(isDeprecated(op, cl))
            {
                out << nl << "@SuppressWarnings(\"deprecation\")";
                break;
            }
        }
        out << nl << "@Override" << nl
            << "default java.util.concurrent.CompletionStage<com.zeroc.Ice.OutputStream> _iceDispatch("
            << "com.zeroc.IceInternal.Incoming in, com.zeroc.Ice.Current current)";
        out.inc();
        out << nl << "throws com.zeroc.Ice.UserException";
        out.dec();
        out << sb;
        out << nl << "int pos = java.util.Arrays.binarySearch(_iceOps, current.operation);";
        out << nl << "if(pos < 0)";
        out << sb;
        out << nl << "throw new "
            << "com.zeroc.Ice.OperationNotExistException(current.id, current.facet, current.operation);";
        out << eb;
        out << sp << nl << "switch(pos)";
        out << sb;
        int i = 0;
        for(StringList::const_iterator q = allOpNames.begin(); q != allOpNames.end(); ++q)
        {
            string opName = *q;

            out << nl << "case " << i++ << ':';
            out << sb;
            if(opName == "ice_id")
            {
                out << nl << "return com.zeroc.Ice.Object._iceD_ice_id(this, in, current);";
            }
            else if(opName == "ice_ids")
            {
                out << nl << "return com.zeroc.Ice.Object._iceD_ice_ids(this, in, current);";
            }
            else if(opName == "ice_isA")
            {
                out << nl << "return com.zeroc.Ice.Object._iceD_ice_isA(this, in, current);";
            }
            else if(opName == "ice_ping")
            {
                out << nl << "return com.zeroc.Ice.Object._iceD_ice_ping(this, in, current);";
            }
            else
            {
                //
                // There's probably a better way to do this.
                //
                for(OperationList::const_iterator t = allOps.begin(); t != allOps.end(); ++t)
                {
                    if((*t)->name() == (*q))
                    {
                        ContainerPtr container = (*t)->container();
                        ClassDefPtr cl = ClassDefPtr::dynamicCast(container);
                        assert(cl);
                        if(cl->scoped() == p->scoped())
                        {
                            out << nl << "return _iceD_" << opName << "(this, in, current);";
                        }
                        else
                        {
                            string base;
                            if(cl->isInterface())
                            {
                                base = getAbsolute(cl, package);
                            }
                            else
                            {
                                base = getAbsolute(cl, package, "_", "Disp");
                            }
                            out << nl << "return " << base << "._iceD_" << opName << "(this, in, current);";
                        }
                        break;
                    }
                }
            }
            out << eb;
        }
        out << eb;
        out << sp << nl << "assert(false);";
        out << nl << "throw new "
            << "com.zeroc.Ice.OperationNotExistException(current.id, current.facet, current.operation);";
        out << eb;

        //
        // Check if we need to generate ice_operationAttributes()
        //

        map<string, int> attributesMap;
        for(OperationList::iterator r = allOps.begin(); r != allOps.end(); ++r)
        {
            int attributes = (*r)->attributes();
            if(attributes != 0)
            {
                attributesMap.insert(map<string, int>::value_type((*r)->name(), attributes));
            }
        }

        if(!attributesMap.empty())
        {
            out << sp << nl << "final static int[] _iceOperationAttributes =";
            out << sb;
            for(StringList::const_iterator q = allOpNames.begin(); q != allOpNames.end();)
            {
                int attributes = 0;
                string opName = *q;
                map<string, int>::iterator it = attributesMap.find(opName);
                if(it != attributesMap.end())
                {
                    attributes = it->second;
                }
                out << nl << attributes;
                if(++q != allOpNames.end())
                {
                    out << ',';
                }
                out  << " // " << opName;
            }
            out << eb << ';';

            out << sp << nl << "@Override" << nl << "default int ice_operationAttributes(String operation)";
            out << sb;
            out << nl << "int pos = java.util.Arrays.binarySearch(_iceOps, operation);";
            out << nl << "if(pos < 0)";
            out << sb;
            out << nl << "return -1;";
            out << eb;
            out << sp << nl << "return _iceOperationAttributes[pos];";
            out << eb;
        }
    }
}

void
Slice::JavaVisitor::writeMarshaling(Output& out, const ClassDefPtr& p)
{
    string name = fixKwd(p->name());
    string package = getPackage(p);
    string scoped = p->scoped();
    ClassList bases = p->bases();
    ClassDefPtr base;
    if(!bases.empty() && !bases.front()->isInterface())
    {
        base = bases.front();
    }

    int iter;
    DataMemberList members = p->dataMembers();
    DataMemberList optionalMembers = p->orderedOptionalDataMembers();
    bool basePreserved = p->inheritsMetaData("preserve-slice");
    bool preserved = p->hasMetaData("preserve-slice");

    if(preserved && !basePreserved)
    {
        out << sp;
        out << nl << "@Override";
        out << nl << "public void _iceWrite(com.zeroc.Ice.OutputStream ostr)";
        out << sb;
        out << nl << "ostr.startValue(_iceSlicedData);";
        out << nl << "_iceWriteImpl(ostr);";
        out << nl << "ostr.endValue();";
        out << eb;

        out << sp;
        out << nl << "@Override";
        out << nl << "public void _iceRead(com.zeroc.Ice.InputStream istr)";
        out << sb;
        out << nl << "istr.startValue();";
        out << nl << "_iceReadImpl(istr);";
        out << nl << "_iceSlicedData = istr.endValue(true);";
        out << eb;
    }

    out << sp;
    out << nl << "@Override";
    out << nl << "protected void _iceWriteImpl(com.zeroc.Ice.OutputStream ostr_)";
    out << sb;
    out << nl << "ostr_.startSlice(ice_staticId(), " << p->compactId() << (!base ? ", true" : ", false") << ");";
    iter = 0;
    for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
    {
        if(!(*d)->optional())
        {
            writeMarshalDataMember(out, package, *d, iter);
        }
    }
    for(DataMemberList::const_iterator d = optionalMembers.begin(); d != optionalMembers.end(); ++d)
    {
        writeMarshalDataMember(out, package, *d, iter);
    }
    out << nl << "ostr_.endSlice();";
    if(base)
    {
        out << nl << "super._iceWriteImpl(ostr_);";
    }
    out << eb;

    DataMemberList classMembers = p->classDataMembers();
    DataMemberList allClassMembers = p->allClassDataMembers();

    out << sp;
    out << nl << "@Override";
    out << nl << "protected void _iceReadImpl(com.zeroc.Ice.InputStream istr_)";
    out << sb;
    out << nl << "istr_.startSlice();";

    iter = 0;
    for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
    {
        if(!(*d)->optional())
        {
            writeUnmarshalDataMember(out, package, *d, iter);
        }
    }
    for(DataMemberList::const_iterator d = optionalMembers.begin(); d != optionalMembers.end(); ++d)
    {
        writeUnmarshalDataMember(out, package, *d, iter);
    }

    out << nl << "istr_.endSlice();";
    if(base)
    {
        out << nl << "super._iceReadImpl(istr_);";
    }
    out << eb;

    if(preserved && !basePreserved)
    {
        out << sp << nl << "protected com.zeroc.Ice.SlicedData _iceSlicedData;";
    }
}

void
Slice::JavaVisitor::writeConstantValue(Output& out, const TypePtr& type, const SyntaxTreeBasePtr& valueType,
                                       const string& value, const string& package)
{
    ConstPtr constant = ConstPtr::dynamicCast(valueType);
    if(constant)
    {
        out << getAbsolute(constant, package) << ".value";
    }
    else
    {
        BuiltinPtr bp;
        EnumPtr ep;
        if((bp = BuiltinPtr::dynamicCast(type)))
        {
            switch(bp->kind())
            {
                case Builtin::KindString:
                {
                    out << "\"" << toStringLiteral(value, "\b\f\n\r\t", "", ShortUCN, 0) << "\"";
                    break;
                }
                case Builtin::KindByte:
                {
                    int i = atoi(value.c_str());
                    if(i > 127)
                    {
                        i -= 256;
                    }
                    out << i; // Slice byte runs from 0-255, Java byte runs from -128 - 127.
                    break;
                }
                case Builtin::KindLong:
                {
                    out << value << "L"; // Need to append "L" modifier for long constants.
                    break;
                }
                case Builtin::KindBool:
                case Builtin::KindShort:
                case Builtin::KindInt:
                case Builtin::KindDouble:
                case Builtin::KindObject:
                case Builtin::KindObjectProxy:
                case Builtin::KindLocalObject:
                case Builtin::KindValue:
                {
                    out << value;
                    break;
                }
                case Builtin::KindFloat:
                {
                    out << value << "F";
                    break;
                }
            }

        }
        else if((ep = EnumPtr::dynamicCast(type)))
        {
            string val = value;
            string::size_type pos = val.rfind(':');
            if(pos != string::npos)
            {
                val.erase(0, pos + 1);
            }
            out << getAbsolute(ep, package) << '.' << fixKwd(val);
        }
        else
        {
            out << value;
        }
    }
}

void
Slice::JavaVisitor::writeDataMemberInitializers(Output& out, const DataMemberList& members, const string& package)
{
    for(DataMemberList::const_iterator p = members.begin(); p != members.end(); ++p)
    {
        TypePtr t = (*p)->type();
        if((*p)->defaultValueType())
        {
            if((*p)->optional())
            {
                string capName = (*p)->name();
                capName[0] = toupper(static_cast<unsigned char>(capName[0]));
                out << nl << "set" << capName << '(';
                writeConstantValue(out, t, (*p)->defaultValueType(), (*p)->defaultValue(), package);
                out << ");";
            }
            else
            {
                out << nl << "this." << fixKwd((*p)->name()) << " = ";
                writeConstantValue(out, t, (*p)->defaultValueType(), (*p)->defaultValue(), package);
                out << ';';
            }
        }
        else
        {
            BuiltinPtr builtin = BuiltinPtr::dynamicCast(t);
            if(builtin && builtin->kind() == Builtin::KindString)
            {
                out << nl << "this." << fixKwd((*p)->name()) << " = \"\";";
            }

            EnumPtr en = EnumPtr::dynamicCast(t);
            if(en)
            {
                string firstEnum = fixKwd(en->getEnumerators().front()->name());
                out << nl << "this." << fixKwd((*p)->name()) << " = " << getAbsolute(en, package) << '.' << firstEnum << ';';
            }

            StructPtr st = StructPtr::dynamicCast(t);
            if(st)
            {
                string memberType = typeToString(st, TypeModeMember, package, (*p)->getMetaData());
                out << nl << "this." << fixKwd((*p)->name()) << " = new " << memberType << "();";
            }
        }
    }
}

StringList
Slice::JavaVisitor::splitComment(const ContainedPtr& p)
{
    StringList result;

    string comment = p->comment();
    string::size_type pos = 0;
    string::size_type nextPos;
    while((nextPos = comment.find_first_of('\n', pos)) != string::npos)
    {
        result.push_back(string(comment, pos, nextPos - pos));
        pos = nextPos + 1;
    }
    string lastLine = string(comment, pos);
    if(lastLine.find_first_not_of(" \t\n\r") != string::npos)
    {
        result.push_back(lastLine);
    }

    return result;
}

Slice::JavaVisitor::DocCommentPtr
Slice::JavaVisitor::parseDocComment(const ContainedPtr& p)
{
    DocCommentPtr c = new DocComment;
    c->deprecated = false;

    //
    // First check metadata for a deprecated tag.
    //
    string deprecateMetadata;
    if(p->findMetaData("deprecate", deprecateMetadata))
    {
        c->deprecated = true;
        if(deprecateMetadata.find("deprecate:") == 0 && deprecateMetadata.size() > 10)
        {
            c->deprecateReason = IceUtilInternal::trim(deprecateMetadata.substr(10));
        }
    }

    const StringList lines = splitComment(p);
    if(lines.empty())
    {
        return c->deprecated ? c : DocCommentPtr(0); // Docs exist if it's deprecated.
    }

    StringList::const_iterator i;
    for(i = lines.begin(); i != lines.end(); ++i)
    {
        const string l = *i;
        if(l[0] == '@')
        {
            break;
        }
        if(!c->overview.empty())
        {
            c->overview += "\n";
        }
        c->overview += l;
    }

    enum State { StateMisc, StateParam, StateThrows, StateReturn, StateDeprecated };
    State state = StateMisc;
    string name;
    const string ws = " \t";
    const string paramTag = "@param";
    const string throwsTag = "@throws";
    const string exceptionTag = "@exception";
    const string returnTag = "@return";
    const string deprecatedTag = "@deprecated";
    for(; i != lines.end(); ++i)
    {
        const string l = *i;
        if(l.find(paramTag) == 0)
        {
            state = StateMisc;
            name.clear();
            string::size_type n = l.find_first_not_of(ws, paramTag.size());
            if(n == string::npos)
            {
                continue; // Malformed line, ignore it.
            }
            string::size_type end = l.find_first_of(ws, n);
            if(end == string::npos)
            {
                continue; // Malformed line, ignore it.
            }
            name = l.substr(n, end - n);
            state = StateParam;
            n = l.find_first_not_of(ws, end);
            if(n != string::npos)
            {
                c->params[name] = l.substr(n); // The first line of the description.
            }
        }
        else if(l.find(throwsTag) == 0 || l.find(exceptionTag) == 0)
        {
            state = StateMisc;
            name.clear();
            string::size_type n =
                l.find_first_not_of(ws, l.find(throwsTag) == 0 ? throwsTag.size() : exceptionTag.size());
            if(n == string::npos)
            {
                continue; // Malformed line, ignore it.
            }
            string::size_type end = l.find_first_of(ws, n);
            if(end == string::npos)
            {
                continue; // Malformed line, ignore it.
            }
            name = l.substr(n, end - n);
            state = StateThrows;
            n = l.find_first_not_of(ws, end);
            if(n != string::npos)
            {
                c->exceptions[name] = l.substr(n); // The first line of the description.
            }
        }
        else if(l.find(returnTag) == 0)
        {
            state = StateMisc;
            name.clear();
            string::size_type n = l.find_first_not_of(ws, returnTag.size());
            if(n == string::npos)
            {
                continue; // Malformed line, ignore it.
            }
            state = StateReturn;
            c->returns = l.substr(n); // The first line of the description.
        }
        else if(l.find(deprecatedTag) == 0)
        {
            state = StateMisc;
            name.clear();
            string::size_type n = l.find_first_not_of(ws, deprecatedTag.size());
            if(n != string::npos)
            {
                c->deprecateReason = l.substr(n); // The first line of the description.
            }
            state = StateDeprecated;
            c->deprecated = true;
        }
        else if(!l.empty())
        {
            if(l[0] == '@')
            {
                //
                // Treat all other tags as miscellaneous comments.
                //
                state = StateMisc;
            }

            switch(state)
            {
            case StateMisc:
                if(!c->misc.empty())
                {
                    c->misc += "\n";
                }
                c->misc += l;
                break;
            case StateParam:
                assert(!name.empty());
                if(c->params.find(name) == c->params.end())
                {
                    c->params[name] = "";
                }
                if(!c->params[name].empty())
                {
                    c->params[name] += "\n";
                }
                c->params[name] += l;
                break;
            case StateThrows:
                assert(!name.empty());
                if(c->exceptions.find(name) == c->exceptions.end())
                {
                    c->exceptions[name] = "";
                }
                if(!c->exceptions[name].empty())
                {
                    c->exceptions[name] += "\n";
                }
                c->exceptions[name] += l;
                break;
            case StateReturn:
                if(!c->returns.empty())
                {
                    c->returns += "\n";
                }
                c->returns += l;
                break;
            case StateDeprecated:
                if(!c->deprecateReason.empty())
                {
                    c->deprecateReason += "\n";
                }
                c->deprecateReason += l;
                break;
            }
        }
    }

    return c;
}

void
Slice::JavaVisitor::writeDocCommentLines(Output& out, const string& text)
{
    //
    // This method emits a block of text, prepending a leading " * " to the second and
    // subsequent lines. We assume the caller prepended a leading " * " for the first
    // line if necessary.
    //
    string::size_type start = 0;
    string::size_type pos;
    const string ws = "\n";
    pos = text.find_first_of(ws);
    if(pos == string::npos)
    {
        out << text;
    }
    else
    {
        string s = IceUtilInternal::trim(text.substr(start, pos - start));
        out << s; // Emit the first line.
        start = pos + 1;
        while((pos = text.find_first_of(ws, start)) != string::npos)
        {
            out << nl << " * " << IceUtilInternal::trim(text.substr(start, pos - start));
            start = pos + 1;
        }
        if(start < text.size())
        {
            out << nl << " * " << IceUtilInternal::trim(text.substr(start));
        }
    }
}

void
Slice::JavaVisitor::writeDocComment(Output& out, const DocCommentPtr& dc)
{
    if(!dc)
    {
        return;
    }

    out << nl << "/**";
    if(!dc->overview.empty())
    {
        out << nl << " * ";
        writeDocCommentLines(out, dc->overview);
    }

    if(!dc->misc.empty())
    {
        out << nl << " * ";
        writeDocCommentLines(out, dc->misc);
    }

    if(!dc->deprecateReason.empty())
    {
        out << nl << " * @deprecated ";
        writeDocCommentLines(out, dc->deprecateReason);
    }

    out << nl << " **/";
}

void
Slice::JavaVisitor::writeDocComment(Output& out, const string& text)
{
    if(!text.empty())
    {
        out << nl << "/**";
        out << nl << " * ";
        writeDocCommentLines(out, text);
        out << nl << " **/";
    }
}

void
Slice::JavaVisitor::writeProxyDocComment(Output& out, const OperationPtr& p, const string& package,
                                         const DocCommentPtr& dc, bool async, bool context)
{
    if(!dc)
    {
        return;
    }

    const string contextParam = " * @param context The Context map to send with the invocation.";

    out << nl << "/**";
    if(!dc->overview.empty())
    {
        out << nl << " * ";
        writeDocCommentLines(out, dc->overview);
    }

    //
    // Show in-params in order of declaration, but only those with docs.
    //
    const ParamDeclList paramList = p->inParameters();
    for(ParamDeclList::const_iterator i = paramList.begin(); i != paramList.end(); ++i)
    {
        const string name = (*i)->name();
        map<string, string>::const_iterator j = dc->params.find(name);
        if(j != dc->params.end() && !j->second.empty())
        {
            out << nl << " * @param " << fixKwd(j->first) << ' ';
            writeDocCommentLines(out, j->second);
        }
    }
    if(context)
    {
        out << nl << contextParam;
    }

    //
    // Handle the return value (if any).
    //
    if(p->returnsMultipleValues())
    {
        const string r = getResultType(p, package, true, false);
        if(async)
        {
            out << nl << " * @return A future that will be completed with an instance of " << r << '.';
        }
        else
        {
            out << nl << " * @return An instance of " << r << '.';
        }
    }
    else if(p->returnType())
    {
        if(!dc->returns.empty())
        {
            out << nl << " * @return ";
            writeDocCommentLines(out, dc->returns);
        }
        else if(async)
        {
            out << nl << " * @return A future that will be completed with the result.";
        }
    }
    else if(!p->outParameters().empty())
    {
        assert(p->outParameters().size() == 1);
        const ParamDeclPtr param = p->outParameters().front();
        map<string, string>::const_iterator j = dc->params.find(param->name());
        if(j != dc->params.end() && !j->second.empty())
        {
            out << nl << " * @return ";
            writeDocCommentLines(out, j->second);
        }
        else if(async)
        {
            out << nl << " * @return A future that will be completed with the result.";
        }
    }
    else if(async)
    {
        //
        // No results but an async proxy operation still returns a future.
        //
        out << nl << " * @return A future that will be completed when the invocation completes.";
    }

    //
    // Async proxy methods don't declare user exceptions.
    //
    if(!async)
    {
        for(map<string, string>::const_iterator i = dc->exceptions.begin(); i != dc->exceptions.end(); ++i)
        {
            out << nl << " * @throws " << fixKwd(i->first) << ' ';
            writeDocCommentLines(out, i->second);
        }
    }

    if(!dc->misc.empty())
    {
        out << nl << " * ";
        writeDocCommentLines(out, dc->misc);
    }

    if(!dc->deprecateReason.empty())
    {
        out << nl << " * @deprecated ";
        writeDocCommentLines(out, dc->deprecateReason);
    }

    out << nl << " **/";
}

void
Slice::JavaVisitor::writeServantDocComment(Output& out, const OperationPtr& p, const string& package,
                                           const DocCommentPtr& dc, bool async)
{
    if(!dc)
    {
        return;
    }

    const ParamDeclList paramList = p->inParameters();
    const string currentParamName = getEscapedParamName(p, "current");
    const string currentParam = " * @param " + currentParamName + " The Current object for the invocation.";

    out << nl << "/**";
    if(!dc->overview.empty())
    {
        out << nl << " * ";
        writeDocCommentLines(out, dc->overview);
    }

    //
    // Show in-params in order of declaration, but only those with docs.
    //
    for(ParamDeclList::const_iterator i = paramList.begin(); i != paramList.end(); ++i)
    {
        const string name = (*i)->name();
        map<string, string>::const_iterator j = dc->params.find(name);
        if(j != dc->params.end() && !j->second.empty())
        {
            out << nl << " * @param " << fixKwd(j->first) << ' ';
            writeDocCommentLines(out, j->second);
        }
    }
    out << nl << currentParam;

    //
    // Handle the return value (if any).
    //
    if(p->returnsMultipleValues())
    {
        const string r = getResultType(p, package, true, false);
        if(async)
        {
            out << nl << " * @return A completion stage that the servant will complete with an instance of " << r
                << '.';
        }
        else
        {
            out << nl << " * @return An instance of " << r << '.';
        }
    }
    else if(p->returnType())
    {
        if(!dc->returns.empty())
        {
            out << nl << " * @return ";
            writeDocCommentLines(out, dc->returns);
        }
        else if(async)
        {
            out << nl << " * @return A completion stage that the servant will complete with the result.";
        }
    }
    else if(!p->outParameters().empty())
    {
        assert(p->outParameters().size() == 1);
        const ParamDeclPtr param = p->outParameters().front();
        map<string, string>::const_iterator j = dc->params.find(param->name());
        if(j != dc->params.end() && !j->second.empty())
        {
            out << nl << " * @return ";
            writeDocCommentLines(out, j->second);
        }
        else if(async)
        {
            out << nl << " * @return A completion stage that the servant will complete with the result.";
        }
    }
    else if(async)
    {
        //
        // No results but an async operation still returns a completion stage.
        //
        out << nl << " * @return A completion stage that the servant will complete when the invocation completes.";
    }

    if(p->hasMetaData("java:UserException") || p->hasMetaData("UserException"))
    {
        out << nl << " * @throws com.zeroc.Ice.UserException";
    }
    else
    {
        for(map<string, string>::const_iterator i = dc->exceptions.begin(); i != dc->exceptions.end(); ++i)
        {
            out << nl << " * @throws " << fixKwd(i->first) << ' ';
            writeDocCommentLines(out, i->second);
        }
    }

    if(!dc->misc.empty())
    {
        out << nl << " * ";
        writeDocCommentLines(out, dc->misc);
    }

    if(!dc->deprecateReason.empty())
    {
        out << nl << " * @deprecated ";
        writeDocCommentLines(out, dc->deprecateReason);
    }

    out << nl << " **/";
}

Slice::Gen::Gen(const string& /*name*/, const string& base, const vector<string>& includePaths, const string& dir) :
    _base(base),
    _includePaths(includePaths),
    _dir(dir)
{
}

Slice::Gen::~Gen()
{
}

void
Slice::Gen::generate(const UnitPtr& p)
{
    JavaGenerator::validateMetaData(p);

    PackageVisitor packageVisitor(_dir);
    p->visit(&packageVisitor, false);

    TypesVisitor typesVisitor(_dir);
    p->visit(&typesVisitor, false);

    CompactIdVisitor compactIdVisitor(_dir);
    p->visit(&compactIdVisitor, false);

    HelperVisitor helperVisitor(_dir);
    p->visit(&helperVisitor, false);

    ProxyVisitor proxyVisitor(_dir);
    p->visit(&proxyVisitor, false);

    DispatcherVisitor dispatcherVisitor(_dir);
    p->visit(&dispatcherVisitor, false);
}

void
Slice::Gen::generateImpl(const UnitPtr& p)
{
    ImplVisitor implVisitor(_dir);
    p->visit(&implVisitor, false);
}

void
Slice::Gen::writeChecksumClass(const string& checksumClass, const string& dir, const ChecksumMap& m)
{
    //
    // Attempt to open the source file for the checksum class.
    //
    JavaOutput out;
    out.openClass(checksumClass, dir);

    //
    // Get the class name.
    //
    string className;
    string::size_type pos = checksumClass.rfind('.');
    if(pos == string::npos)
    {
        className = checksumClass;
    }
    else
    {
        className = checksumClass.substr(pos + 1);
    }

    //
    // Emit the class.
    //
    out << sp << nl << "public class " << className;
    out << sb;

    //
    // Use a static initializer to populate the checksum map.
    //
    out << sp << nl << "public static final java.util.Map<String, String> checksums;";
    out << sp << nl << "static";
    out << sb;
    out << nl << "java.util.Map<String, String> map = new java.util.HashMap<>();";
    for(ChecksumMap::const_iterator p = m.begin(); p != m.end(); ++p)
    {
        out << nl << "map.put(\"" << p->first << "\", \"";
        ostringstream str;
        str.flags(ios_base::hex);
        str.fill('0');
        for(vector<unsigned char>::const_iterator q = p->second.begin(); q != p->second.end(); ++q)
        {
            str << static_cast<int>(*q);
        }
        out << str.str() << "\");";
    }
    out << nl << "checksums = java.util.Collections.unmodifiableMap(map);";

    out << eb;
    out << eb;
    out << nl;
}

Slice::Gen::PackageVisitor::PackageVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

bool
Slice::Gen::PackageVisitor::visitModuleStart(const ModulePtr& p)
{
    string prefix = getPackagePrefix(p);
    if(!prefix.empty())
    {
        string markerClass = prefix + "." + fixKwd(p->name()) + "._Marker";
        open(markerClass, p->file());

        Output& out = output();

        out << sp << nl << "interface _Marker";
        out << sb;
        out << eb;

        close();
    }
    return false;
}

Slice::Gen::TypesVisitor::TypesVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

bool
Slice::Gen::TypesVisitor::visitClassDefStart(const ClassDefPtr& p)
{
    string name = p->name();
    ClassList bases = p->bases();
    ClassDefPtr baseClass;
    if(!bases.empty() && !bases.front()->isInterface())
    {
        baseClass = bases.front();
    }

    string package = getPackage(p);
    string absolute = getAbsolute(p);
    DataMemberList members = p->dataMembers();
    DataMemberList allDataMembers = p->allDataMembers();

    open(absolute, p->file());

    Output& out = output();

    //
    // Check for java:implements metadata.
    //
    const StringList metaData = p->getMetaData();
    static const string prefix = "java:implements:";
    StringList implements;
    for(StringList::const_iterator q = metaData.begin(); q != metaData.end(); ++q)
    {
        if(q->find(prefix) == 0)
        {
            implements.push_back(q->substr(prefix.size()));
        }
    }

    DocCommentPtr dc = parseDocComment(p);

    //
    // Slice interfaces map to Java interfaces.
    //
    out << sp;
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    if(p->isInterface())
    {
        out << nl << "public interface " << fixKwd(name);
        ClassList::const_iterator q = bases.begin();
        StringList::const_iterator r = implements.begin();

        if(!p->isLocal() || !bases.empty() || !implements.empty())
        {
            out << " extends ";
        }
        out.useCurrentPosAsIndent();
        if(!p->isLocal() && bases.empty())
        {
            out << "com.zeroc.Ice.Object";
        }
        else if(q != bases.end())
        {
            out << getAbsolute(*q++, package);
        }
        else if(r != implements.end())
        {
            out << *r++;
        }

        for(;q != bases.end(); ++q)
        {
            out << ',' << nl << getAbsolute(*q, package);
        }
        for(; r != implements.end(); ++r)
        {
            out << ',' << nl << *r;
        }
        out.restoreIndent();
    }
    else
    {
        out << nl << "public ";
        if((p->isLocal() && !p->allOperations().empty()) || !implements.empty())
        {
            out << "abstract ";
        }
        out << "class " << fixKwd(name);
        out.useCurrentPosAsIndent();

        if(baseClass)
        {
            out << " extends " << getAbsolute(baseClass, package);
            bases.pop_front();
        }
        else if(!p->isLocal())
        {
            out << " extends com.zeroc.Ice.Value";
        }
        else
        {
            implements.push_back("java.lang.Cloneable");
        }

        if(!implements.empty())
        {
            if(baseClass || !p->isLocal())
            {
                out << nl;
            }

            out << " implements ";
            out.useCurrentPosAsIndent();
            for(StringList::const_iterator q = implements.begin(); q != implements.end(); ++q)
            {
                if(q != implements.begin())
                {
                    out << ',' << nl;
                }
                out << *q;
            }
            out.restoreIndent();
        }

        out.restoreIndent();
    }

    out << sb;

    if(!p->isInterface() && !allDataMembers.empty())
    {
        bool hasOptionalMembers = false;
        bool hasRequiredMembers = false;

        for(DataMemberList::const_iterator d = allDataMembers.begin(); d != allDataMembers.end(); ++d)
        {
            if((*d)->optional())
            {
                hasOptionalMembers = true;
            }
            else
            {
                hasRequiredMembers = true;
            }
        }

        //
        // Default constructor.
        //
        out << sp;
        out << nl << "public " << fixKwd(name) << "()";
        out << sb;
        if(baseClass)
        {
            out << nl << "super();";
        }
        writeDataMemberInitializers(out, members, package);
        out << eb;

        //
        // A method cannot have more than 255 parameters (including the implicit "this" argument).
        //
        if(allDataMembers.size() < 255)
        {
            DataMemberList baseDataMembers;
            if(baseClass)
            {
                baseDataMembers = baseClass->allDataMembers();
            }

            if(hasRequiredMembers && hasOptionalMembers)
            {
                //
                // Generate a constructor accepting parameters for just the required members.
                //
                out << sp << nl << "public " << fixKwd(name) << spar;
                vector<string> paramDecl;
                for(DataMemberList::const_iterator d = allDataMembers.begin(); d != allDataMembers.end(); ++d)
                {
                    if(!(*d)->optional())
                    {
                        string memberName = fixKwd((*d)->name());
                        string memberType = typeToString((*d)->type(), TypeModeMember, package, (*d)->getMetaData());
                        paramDecl.push_back(memberType + " " + memberName);
                    }
                }
                out << paramDecl << epar;
                out << sb;
                if(!baseDataMembers.empty())
                {
                    bool hasBaseRequired = false;
                    for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                    {
                        if(!(*d)->optional())
                        {
                            hasBaseRequired = true;
                            break;
                        }
                    }
                    if(hasBaseRequired)
                    {
                        out << nl << "super" << spar;
                        vector<string> baseParamNames;
                        for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                        {
                            if(!(*d)->optional())
                            {
                                baseParamNames.push_back(fixKwd((*d)->name()));
                            }
                        }
                        out << baseParamNames << epar << ';';
                    }
                }

                for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
                {
                    if(!(*d)->optional())
                    {
                        string paramName = fixKwd((*d)->name());
                        out << nl << "this." << paramName << " = " << paramName << ';';
                    }
                }
                writeDataMemberInitializers(out, p->orderedOptionalDataMembers(), package);
                out << eb;
            }

            //
            // Generate a constructor accepting parameters for all members.
            //
            out << sp << nl << "public " << fixKwd(name) << spar;
            vector<string> paramDecl;
            for(DataMemberList::const_iterator d = allDataMembers.begin(); d != allDataMembers.end(); ++d)
            {
                string memberName = fixKwd((*d)->name());
                string memberType = typeToString((*d)->type(), TypeModeMember, package, (*d)->getMetaData());
                paramDecl.push_back(memberType + " " + memberName);
            }
            out << paramDecl << epar;
            out << sb;
            if(baseClass && allDataMembers.size() != members.size())
            {
                out << nl << "super" << spar;
                vector<string> baseParamNames;
                for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                {
                    baseParamNames.push_back(fixKwd((*d)->name()));
                }
                out << baseParamNames << epar << ';';
            }
            for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
            {
                string paramName = fixKwd((*d)->name());
                if((*d)->optional())
                {
                    string capName = paramName;
                    capName[0] = toupper(static_cast<unsigned char>(capName[0]));
                    out << nl << "set" << capName << '(' << paramName << ");";
                }
                else
                {
                    out << nl << "this." << paramName << " = " << paramName << ';';
                }
            }
            out << eb;
        }
    }

    return true;
}

void
Slice::Gen::TypesVisitor::visitClassDefEnd(const ClassDefPtr& p)
{
    Output& out = output();

    ClassList bases = p->bases();
    ClassDefPtr baseClass;
    if(!bases.empty() && !bases.front()->isInterface())
    {
        baseClass = bases.front();
    }

    string name = fixKwd(p->name());

    if(!p->isInterface())
    {
        out << sp << nl << "public " << name << " clone()";
        out << sb;

        if(p->isLocal() && !baseClass)
        {
            out << nl << name << " c = null;";
            out << nl << "try";
            out << sb;
            out << nl << "c = (" << name << ")super.clone();";
            out << eb;
            out << nl << "catch(CloneNotSupportedException ex)";
            out << sb;
            out << nl << "assert false; // impossible";
            out << eb;
            out << nl << "return c;";

        }
        else
        {
            out << nl << "return (" << name << ")super.clone();";
        }
        out << eb;
    }

    if(!p->isLocal())
    {
        if(!p->isInterface())
        {
            out << sp << nl << "public static String ice_staticId()";
            out << sb;
            out << nl << "return \"" << p->scoped() << "\";";
            out << eb;

            out << sp << nl << "@Override";
            out << nl << "public String ice_id()";
            out << sb;
            out << nl << "return ice_staticId();";
            out << eb;
        }
    }

    if(!p->isInterface())
    {
        out << sp << nl << "public static final long serialVersionUID = ";
        string serialVersionUID;
        if(p->findMetaData("java:serialVersionUID", serialVersionUID))
        {
            string::size_type pos = serialVersionUID.rfind(":") + 1;
            if(pos == string::npos)
            {
                ostringstream os;
                os << "ignoring invalid serialVersionUID for class `" << p->scoped() << "'; generating default value";
                emitWarning("", "", os.str());
                out << computeSerialVersionUUID(p);
            }
            else
            {
                Int64 v = 0;
                serialVersionUID = serialVersionUID.substr(pos);
                if(serialVersionUID != "0")
                {
                    if(!stringToInt64(serialVersionUID, v)) // conversion error
                    {
                        ostringstream os;
                        os << "ignoring invalid serialVersionUID for class `" << p->scoped()
                           << "'; generating default value";
                        emitWarning("", "", os.str());
                        out << computeSerialVersionUUID(p);
                    }
                }
                out << v;
            }
        }
        else
        {
            out << computeSerialVersionUUID(p);
        }
        out << "L;";
    }

    if(!p->isLocal())
    {
        if(p->isInterface())
        {
            writeDispatch(out, p);
        }
        else
        {
            writeMarshaling(out, p);
        }
    }

    out << eb;
    close();
}

void
Slice::Gen::TypesVisitor::visitOperation(const OperationPtr& p)
{
    //
    // Generate the operation signature for a servant.
    //

    ClassDefPtr cl = ClassDefPtr::dynamicCast(p->container());
    assert(cl);

    const string package = getPackage(cl);

    Output& out = output();

    DocCommentPtr dc = parseDocComment(p);

    //
    // Generate the "Result" type needed by operations that return multiple values.
    //
    if(p->returnsMultipleValues())
    {
        writeResultType(out, p, package, dc);
    }

    //
    // The "MarshaledResult" type is generated in the servant interface.
    //
    if(cl->isInterface() && p->hasMarshaledResult())
    {
        writeMarshaledResultType(out, p, package, dc);
    }

    if(cl->isLocal())
    {
        const string opname = p->name();
        vector<string> params = getParams(p, package);

        const string retS = getResultType(p, package, false, false);

        ExceptionList throws = p->throws();
        throws.sort();
        throws.unique();
        out << sp;

        writeProxyDocComment(out, p, package, dc, false, false);
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl;
        if(!cl->isInterface())
        {
            out << "public abstract ";
        }
        out << retS << ' ' << fixKwd(opname) << spar << params << epar;
        writeThrowsClause(package, throws, p);
        out << ';';

        //
        // Generate asynchronous API for local operations marked with "async-oneway" metadata.
        //
        if(cl->hasMetaData("async-oneway") || p->hasMetaData("async-oneway"))
        {
            out << sp;
            writeProxyDocComment(out, p, package, dc, true, false);
            if(dc && dc->deprecated)
            {
                out << nl << "@Deprecated";
            }
            out << nl;
            if(!cl->isInterface())
            {
                out << "public abstract ";
            }
            out << getFutureType(p, package) << ' ' << opname << "Async" << spar << params << epar << ';';
        }
    }
}

bool
Slice::Gen::TypesVisitor::visitExceptionStart(const ExceptionPtr& p)
{
    string name = fixKwd(p->name());
    string scoped = p->scoped();
    ExceptionPtr base = p->base();
    string package = getPackage(p);
    string absolute = getAbsolute(p);
    DataMemberList members = p->dataMembers();
    DataMemberList allDataMembers = p->allDataMembers();

    open(absolute, p->file());

    Output& out = output();

    out << sp;

    DocCommentPtr dc = parseDocComment(p);
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }

    out << nl << "public class " << name << " extends ";

    if(!base)
    {
        if(p->isLocal())
        {
            out << "com.zeroc.Ice.LocalException";
        }
        else
        {
            out << "com.zeroc.Ice.UserException";
        }
    }
    else
    {
        out << getAbsolute(base, package);
    }
    out << sb;

    //
    // Default constructor.
    //
    out << sp;
    out << nl << "public " << name << "()";
    out << sb;
    if(base)
    {
        out << nl << "super();";
    }
    writeDataMemberInitializers(out, members, package);
    out << eb;

    out << sp;
    out << nl << "public " << name << "(Throwable cause)";
    out << sb;
    out << nl << "super(cause);";
    writeDataMemberInitializers(out, members, package);
    out << eb;

    bool hasOptionalMembers = false;
    bool hasRequiredMembers = false;
    for(DataMemberList::const_iterator d = allDataMembers.begin(); d != allDataMembers.end(); ++d)
    {
        if((*d)->optional())
        {
            hasOptionalMembers = true;
        }
        else
        {
            hasRequiredMembers = true;
        }
    }

    if(!allDataMembers.empty())
    {
        DataMemberList baseDataMembers;
        if(base)
        {
            baseDataMembers = base->allDataMembers();
        }

        //
        // A method cannot have more than 255 parameters (including the implicit "this" argument).
        //
        if(allDataMembers.size() < 255)
        {
            if(hasRequiredMembers && hasOptionalMembers)
            {
                bool hasBaseRequired = false;
                for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                {
                    if(!(*d)->optional())
                    {
                        hasBaseRequired = true;
                        break;
                    }
                }

                DataMemberList optionalMembers = p->orderedOptionalDataMembers();

                //
                // Generate a constructor accepting parameters for just the required members.
                //
                out << sp << nl << "public " << name << spar;
                vector<string> paramDecl;
                for(DataMemberList::const_iterator d = allDataMembers.begin(); d != allDataMembers.end(); ++d)
                {
                    if(!(*d)->optional())
                    {
                        string memberName = fixKwd((*d)->name());
                        string memberType = typeToString((*d)->type(), TypeModeMember, package, (*d)->getMetaData(),
                                                         true, false, p->isLocal());
                        paramDecl.push_back(memberType + " " + memberName);
                    }
                }
                out << paramDecl << epar;
                out << sb;
                if(!baseDataMembers.empty())
                {
                    if(hasBaseRequired)
                    {
                        out << nl << "super" << spar;
                        vector<string> baseParamNames;
                        for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                        {
                            if(!(*d)->optional())
                            {
                                baseParamNames.push_back(fixKwd((*d)->name()));
                            }
                        }
                        out << baseParamNames << epar << ';';
                    }
                }

                for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
                {
                    if(!(*d)->optional())
                    {
                        string paramName = fixKwd((*d)->name());
                        out << nl << "this." << paramName << " = " << paramName << ';';
                    }
                }
                writeDataMemberInitializers(out, optionalMembers, package);
                out << eb;

                //
                // Create constructor that takes all data members plus a Throwable.
                //
                if(allDataMembers.size() < 254)
                {
                    const string causeParamName = getEscapedParamName(allDataMembers, "cause");

                    paramDecl.push_back("Throwable " + causeParamName);
                    out << sp << nl << "public " << name << spar;
                    out << paramDecl << epar;
                    out << sb;
                    if(hasBaseRequired)
                    {
                        out << nl << "super" << spar;
                        vector<string> baseParamNames;
                        for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                        {
                            if(!(*d)->optional())
                            {
                                baseParamNames.push_back(fixKwd((*d)->name()));
                            }
                        }
                        baseParamNames.push_back(causeParamName);
                        out << baseParamNames << epar << ';';
                    }
                    else
                    {
                        out << nl << "super(" << causeParamName << ");";
                    }
                    for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
                    {
                        if(!(*d)->optional())
                        {
                            string paramName = fixKwd((*d)->name());
                            out << nl << "this." << paramName << " = " << paramName << ';';
                        }
                    }
                    writeDataMemberInitializers(out, optionalMembers, package);
                    out << eb;
                }
            }

            out << sp << nl << "public " << name << spar;
            vector<string> paramDecl;
            for(DataMemberList::const_iterator d = allDataMembers.begin(); d != allDataMembers.end(); ++d)
            {
                string memberName = fixKwd((*d)->name());
                string memberType = typeToString((*d)->type(), TypeModeMember, package, (*d)->getMetaData(), true,
                                                 false, p->isLocal());
                paramDecl.push_back(memberType + " " + memberName);
            }
            out << paramDecl << epar;
            out << sb;
            if(base && allDataMembers.size() != members.size())
            {
                out << nl << "super" << spar;
                vector<string> baseParamNames;
                DataMemberList baseDataMembers = base->allDataMembers();
                for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                {
                    baseParamNames.push_back(fixKwd((*d)->name()));
                }

                out << baseParamNames << epar << ';';
            }
            for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
            {
                string paramName = fixKwd((*d)->name());
                if((*d)->optional())
                {
                    string capName = paramName;
                    capName[0] = toupper(static_cast<unsigned char>(capName[0]));
                    out << nl << "set" << capName << '(' << paramName << ");";
                }
                else
                {
                    out << nl << "this." << paramName << " = " << paramName << ';';
                }
            }
            out << eb;

            //
            // Create constructor that takes all data members plus a Throwable
            //
            if(allDataMembers.size() < 254)
            {
                const string causeParamName = getEscapedParamName(allDataMembers, "cause");

                paramDecl.push_back("Throwable " + causeParamName);
                out << sp << nl << "public " << name << spar;
                out << paramDecl << epar;
                out << sb;
                if(!base)
                {
                    out << nl << "super(" << causeParamName << ");";
                }
                else
                {
                    out << nl << "super" << spar;
                    vector<string> baseParamNames;
                    DataMemberList baseDataMembers = base->allDataMembers();
                    for(DataMemberList::const_iterator d = baseDataMembers.begin(); d != baseDataMembers.end(); ++d)
                    {
                        baseParamNames.push_back(fixKwd((*d)->name()));
                    }
                    baseParamNames.push_back(causeParamName);
                    out << baseParamNames << epar << ';';
                }
                for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
                {
                    string paramName = fixKwd((*d)->name());
                    if((*d)->optional())
                    {
                        string capName = paramName;
                        capName[0] = toupper(static_cast<unsigned char>(capName[0]));
                        out << nl << "set" << capName << '(' << paramName << ");";
                    }
                    else
                    {
                        out << nl << "this." << paramName << " = " << paramName << ';';
                    }
                }
                out << eb;
            }
        }
    }

    out << sp << nl << "public String ice_id()";
    out << sb;
    out << nl << "return \"" << scoped << "\";";
    out << eb;

    return true;
}

void
Slice::Gen::TypesVisitor::visitExceptionEnd(const ExceptionPtr& p)
{
    Output& out = output();

    if(!p->isLocal())
    {
        string name = fixKwd(p->name());
        string scoped = p->scoped();
        string package = getPackage(p);
        ExceptionPtr base = p->base();
        bool basePreserved = p->inheritsMetaData("preserve-slice");
        bool preserved = p->hasMetaData("preserve-slice");

        DataMemberList members = p->dataMembers();
        DataMemberList optionalMembers = p->orderedOptionalDataMembers();
        int iter;

        if(preserved && !basePreserved)
        {
            out << sp;
            out << nl << "@Override";
            out << nl << "public void _write(com.zeroc.Ice.OutputStream ostr)";
            out << sb;
            out << nl << "ostr.startException(_slicedData);";
            out << nl << "_writeImpl(ostr);";
            out << nl << "ostr.endException();";
            out << eb;

            out << sp;
            out << nl << "@Override";
            out << nl << "public void _read(com.zeroc.Ice.InputStream istr)";
            out << sb;
            out << nl << "istr.startException();";
            out << nl << "_readImpl(istr);";
            out << nl << "_slicedData = istr.endException(true);";
            out << eb;
        }

        out << sp;
        out << nl << "@Override";
        out << nl << "protected void _writeImpl(com.zeroc.Ice.OutputStream ostr_)";
        out << sb;
        out << nl << "ostr_.startSlice(\"" << scoped << "\", -1, " << (!base ? "true" : "false") << ");";
        iter = 0;
        for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
        {
            if(!(*d)->optional())
            {
                writeMarshalDataMember(out, package, *d, iter);
            }
        }
        for(DataMemberList::const_iterator d = optionalMembers.begin(); d != optionalMembers.end(); ++d)
        {
            writeMarshalDataMember(out, package, *d, iter);
        }
        out << nl << "ostr_.endSlice();";
        if(base)
        {
            out << nl << "super._writeImpl(ostr_);";
        }
        out << eb;

        DataMemberList classMembers = p->classDataMembers();
        DataMemberList allClassMembers = p->allClassDataMembers();

        out << sp;
        out << nl << "@Override";
        out << nl << "protected void _readImpl(com.zeroc.Ice.InputStream istr_)";
        out << sb;
        out << nl << "istr_.startSlice();";
        iter = 0;
        for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
        {
            if(!(*d)->optional())
            {
                writeUnmarshalDataMember(out, package, *d, iter);
            }
        }
        for(DataMemberList::const_iterator d = optionalMembers.begin(); d != optionalMembers.end(); ++d)
        {
            writeUnmarshalDataMember(out, package, *d, iter);
        }
        out << nl << "istr_.endSlice();";
        if(base)
        {
            out << nl << "super._readImpl(istr_);";
        }
        out << eb;

        if(p->usesClasses(false))
        {
            if(!base || (base && !base->usesClasses(false)))
            {
                out << sp;
                out << nl << "@Override";
                out << nl << "public boolean _usesClasses()";
                out << sb;
                out << nl << "return true;";
                out << eb;
            }
        }

        if(preserved && !basePreserved)
        {
            out << sp << nl << "protected com.zeroc.Ice.SlicedData _slicedData;";
        }
    }

    out << sp << nl << "public static final long serialVersionUID = ";
    string serialVersionUID;
    if(p->findMetaData("java:serialVersionUID", serialVersionUID))
    {
        string::size_type pos = serialVersionUID.rfind(":") + 1;
        if(pos == string::npos)
        {
            ostringstream os;
            os << "ignoring invalid serialVersionUID for exception `" << p->scoped() << "'; generating default value";
            emitWarning("", "", os.str());
            out << computeSerialVersionUUID(p);
        }
        else
        {
            Int64 v = 0;
            serialVersionUID = serialVersionUID.substr(pos);
            if(serialVersionUID != "0")
            {
                if(!stringToInt64(serialVersionUID, v)) // conversion error
                {
                    ostringstream os;
                    os << "ignoring invalid serialVersionUID for exception `" << p->scoped()
                       << "'; generating default value";
                    emitWarning("", "", os.str());
                    out << computeSerialVersionUUID(p);
                }
            }
            out << v;
        }
    }
    else
    {
        out << computeSerialVersionUUID(p);
    }
    out << "L;";

    out << eb;
    close();
}

bool
Slice::Gen::TypesVisitor::visitStructStart(const StructPtr& p)
{
    string name = fixKwd(p->name());
    string absolute = getAbsolute(p);

    open(absolute, p->file());

    Output& out = output();

    //
    // Check for java:implements metadata.
    //
    const StringList metaData = p->getMetaData();
    static const string prefix = "java:implements:";
    StringList implements;
    for(StringList::const_iterator q = metaData.begin(); q != metaData.end(); ++q)
    {
        if(q->find(prefix) == 0)
        {
            implements.push_back(q->substr(prefix.size()));
        }
    }

    out << sp;

    DocCommentPtr dc = parseDocComment(p);
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }

    out << nl << "public class " << name << " implements ";
    out.useCurrentPosAsIndent();
    out << "java.lang.Cloneable";
    if(!p->isLocal())
    {
        out << "," << nl << "java.io.Serializable";
    }
    for(StringList::const_iterator q = implements.begin(); q != implements.end(); ++q)
    {
        out << "," << nl << *q;
    }
    out << sb;

    return true;
}

void
Slice::Gen::TypesVisitor::visitStructEnd(const StructPtr& p)
{
    string package = getPackage(p);

    Output& out = output();

    DataMemberList members = p->dataMembers();
    int iter;

    string name = fixKwd(p->name());
    string typeS = typeToString(p, TypeModeIn, package);

    out << sp << nl << "public " << name << "()";
    out << sb;
    writeDataMemberInitializers(out, members, package);
    out << eb;

    //
    // A method cannot have more than 255 parameters (including the implicit "this" argument).
    //
    if(members.size() < 255)
    {
        vector<string> paramDecl;
        vector<string> paramNames;
        for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
        {
            string memberName = fixKwd((*d)->name());
            string memberType = typeToString((*d)->type(), TypeModeMember, package, (*d)->getMetaData(), true, false,
                                             p->isLocal());
            paramDecl.push_back(memberType + " " + memberName);
            paramNames.push_back(memberName);
        }

        out << sp << nl << "public " << name << spar << paramDecl << epar;
        out << sb;
        for(vector<string>::const_iterator i = paramNames.begin(); i != paramNames.end(); ++i)
        {
            out << nl << "this." << *i << " = " << *i << ';';
        }
        out << eb;
    }

    out << sp << nl << "public boolean equals(java.lang.Object rhs)";
    out << sb;
    out << nl << "if(this == rhs)";
    out << sb;
    out << nl << "return true;";
    out << eb;
    out << nl << typeS << " r = null;";
    out << nl << "if(rhs instanceof " << typeS << ")";
    out << sb;
    out << nl << "r = (" << typeS << ")rhs;";
    out << eb;
    out << sp << nl << "if(r != null)";
    out << sb;
    for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
    {
        string memberName = fixKwd((*d)->name());
        BuiltinPtr b = BuiltinPtr::dynamicCast((*d)->type());
        if(b)
        {
            switch(b->kind())
            {
                case Builtin::KindByte:
                case Builtin::KindBool:
                case Builtin::KindShort:
                case Builtin::KindInt:
                case Builtin::KindLong:
                case Builtin::KindFloat:
                case Builtin::KindDouble:
                {
                    out << nl << "if(this." << memberName << " != r." << memberName << ')';
                    out << sb;
                    out << nl << "return false;";
                    out << eb;
                    break;
                }

                case Builtin::KindString:
                case Builtin::KindObject:
                case Builtin::KindObjectProxy:
                case Builtin::KindLocalObject:
                case Builtin::KindValue:
                {
                    out << nl << "if(this." << memberName << " != r." << memberName << ')';
                    out << sb;
                    out << nl << "if(this." << memberName << " == null || r." << memberName << " == null || !this."
                        << memberName << ".equals(r." << memberName << "))";
                    out << sb;
                    out << nl << "return false;";
                    out << eb;
                    out << eb;
                    break;
                }
            }
        }
        else
        {
            //
            // We treat sequences differently because the native equals() method for
            // a Java array does not perform a deep comparison. If the mapped type
            // is not overridden via metadata, we use the helper method
            // java.util.Arrays.equals() to compare native arrays.
            //
            // For all other types, we can use the native equals() method.
            //
            SequencePtr seq = SequencePtr::dynamicCast((*d)->type());
            if(seq)
            {
                if(hasTypeMetaData(seq, (*d)->getMetaData()))
                {
                    out << nl << "if(this." << memberName << " != r." << memberName << ')';
                    out << sb;
                    out << nl << "if(this." << memberName << " == null || r." << memberName << " == null || !this."
                        << memberName << ".equals(r." << memberName << "))";
                    out << sb;
                    out << nl << "return false;";
                    out << eb;
                    out << eb;
                }
                else
                {
                    //
                    // Arrays.equals() handles null values.
                    //
                    out << nl << "if(!java.util.Arrays.equals(this." << memberName << ", r." << memberName << "))";
                    out << sb;
                    out << nl << "return false;";
                    out << eb;
                }
            }
            else
            {
                out << nl << "if(this." << memberName << " != r." << memberName << ')';
                out << sb;
                out << nl << "if(this." << memberName << " == null || r." << memberName << " == null || !this."
                    << memberName << ".equals(r." << memberName << "))";
                out << sb;
                out << nl << "return false;";
                out << eb;
                out << eb;
            }
        }
    }
    out << sp << nl << "return true;";
    out << eb;
    out << sp << nl << "return false;";
    out << eb;

    out << sp << nl << "public int hashCode()";
    out << sb;
    out << nl << "int h_ = 5381;";
    out << nl << "h_ = com.zeroc.IceInternal.HashUtil.hashAdd(h_, \"" << p->scoped() << "\");";
    iter = 0;
    for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
    {
        string memberName = fixKwd((*d)->name());
        out << nl << "h_ = com.zeroc.IceInternal.HashUtil.hashAdd(h_, " << memberName << ");";
    }
    out << nl << "return h_;";
    out << eb;

    out << sp << nl << "public " << name << " clone()";
    out << sb;
    out << nl << name << " c = null;";
    out << nl << "try";
    out << sb;
    out << nl << "c = (" << name << ")super.clone();";
    out << eb;
    out << nl << "catch(CloneNotSupportedException ex)";
    out << sb;
    out << nl << "assert false; // impossible";
    out << eb;
    out << nl << "return c;";
    out << eb;

    if(!p->isLocal())
    {
        out << sp << nl << "public void ice_writeMembers(com.zeroc.Ice.OutputStream ostr)";
        out << sb;
        iter = 0;
        for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
        {
            writeMarshalDataMember(out, package, *d, iter, true);
        }
        out << eb;

        DataMemberList classMembers = p->classDataMembers();

        out << sp << nl << "public void ice_readMembers(com.zeroc.Ice.InputStream istr)";
        out << sb;
        iter = 0;
        for(DataMemberList::const_iterator d = members.begin(); d != members.end(); ++d)
        {
            writeUnmarshalDataMember(out, package, *d, iter, true);
        }
        out << eb;

        out << sp << nl << "static public void ice_write(com.zeroc.Ice.OutputStream ostr, " << name << " v)";
        out << sb;
        out << nl << "if(v == null)";
        out << sb;
        out << nl << "_nullMarshalValue.ice_writeMembers(ostr);";
        out << eb;
        out << nl << "else";
        out << sb;
        out << nl << "v.ice_writeMembers(ostr);";
        out << eb;
        out << eb;

        out << sp << nl << "static public " << name << " ice_read(com.zeroc.Ice.InputStream istr, " << name << " v)";
        out << sb;
        out << nl << "if(v == null)";
        out << sb;
        out << nl << " v = new " << name << "();";
        out << eb;
        out << nl << "v.ice_readMembers(istr);";
        out << nl << "return v;";
        out << eb;

        out << nl << nl << "private static final " << name << " _nullMarshalValue = new " << name << "();";
    }

    out << sp << nl << "public static final long serialVersionUID = ";
    string serialVersionUID;
    if(p->findMetaData("java:serialVersionUID", serialVersionUID))
    {
        string::size_type pos = serialVersionUID.rfind(":") + 1;
        if(pos == string::npos)
        {
            ostringstream os;
            os << "ignoring invalid serialVersionUID for struct `" << p->scoped() << "'; generating default value";
            emitWarning("", "", os.str());
            out << computeSerialVersionUUID(p);
        }
        else
        {
            Int64 v = 0;
            serialVersionUID = serialVersionUID.substr(pos);
            if(serialVersionUID != "0")
            {
                if(!stringToInt64(serialVersionUID, v)) // conversion error
                {
                    ostringstream os;
                    os << "ignoring invalid serialVersionUID for struct `" << p->scoped()
                       << "'; generating default value";
                    emitWarning("", "", os.str());
                    out << computeSerialVersionUUID(p);
                }
            }
            out << v;
        }
    }
    else
    {
        out << computeSerialVersionUUID(p);
    }
    out << "L;";

    out << eb;
    close();
}

void
Slice::Gen::TypesVisitor::visitDataMember(const DataMemberPtr& p)
{
    const ContainerPtr container = p->container();
    const ClassDefPtr cls = ClassDefPtr::dynamicCast(container);
    const StructPtr st = StructPtr::dynamicCast(container);
    const ExceptionPtr ex = ExceptionPtr::dynamicCast(container);
    const ContainedPtr contained = ContainedPtr::dynamicCast(container);

    const string name = fixKwd(p->name());
    const StringList metaData = p->getMetaData();
    const bool getSet = p->hasMetaData(_getSetMetaData) || contained->hasMetaData(_getSetMetaData);
    const bool optional = p->optional();
    const TypePtr type = p->type();
    const BuiltinPtr b = BuiltinPtr::dynamicCast(type);
    const bool classType = isValue(type);

    bool local;
    if(cls)
    {
        local = cls->isLocal();
    }
    else if(st)
    {
        local = st->isLocal();
    }
    else
    {
        assert(ex);
        local = ex->isLocal();
    }

    const string s = typeToString(type, TypeModeMember, getPackage(contained), metaData, true, false, local);

    Output& out = output();

    out << sp;

    DocCommentPtr dc = parseDocComment(p);
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }

    //
    // Access visibility for class data members can be controlled by metadata.
    // If none is specified, the default is public.
    //
    if(cls && (p->hasMetaData("protected") || contained->hasMetaData("protected")))
    {
        out << nl << "protected " << s << ' ' << name << ';';
    }
    else if(optional)
    {
        out << nl << "private " << s << ' ' << name << ';';
    }
    else
    {
        out << nl << "public " << s << ' ' << name << ';';
    }

    if(optional)
    {
        out << nl << "private boolean _" << p->name() << ';';
    }

    //
    // Getter/Setter.
    //
    if(getSet || optional)
    {
        string capName = p->name();
        capName[0] = toupper(static_cast<unsigned char>(capName[0]));

        //
        // If container is a class, get all of its operations so that we can check for conflicts.
        //
        OperationList ops;
        string file, line;
        if(cls)
        {
            ops = cls->allOperations();
            file = p->file();
            line = p->line();
            if(!validateMethod(ops, "get" + capName, 0, file, line) ||
               !validateMethod(ops, "set" + capName, 1, file, line))
            {
                return;
            }
            if(optional &&
               (!validateMethod(ops, "has" + capName, 0, file, line) ||
                !validateMethod(ops, "clear" + capName, 0, file, line) ||
                !validateMethod(ops, "optional" + capName, 0, file, line)))
            {
                return;
            }
        }

        //
        // Getter.
        //
        out << sp;
        writeDocComment(out, dc);
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "public " << s << " get" << capName << "()";
        out << sb;
        if(optional)
        {
            out << nl << "if(!_" << p->name() << ')';
            out << sb;
            out << nl << "throw new java.util.NoSuchElementException(\"" << name << " is not set\");";
            out << eb;
        }
        out << nl << "return " << name << ';';
        out << eb;

        //
        // Setter.
        //
        out << sp;
        writeDocComment(out, dc);
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "public void set" << capName << '(' << s << " " << name << ')';
        out << sb;
        if(optional)
        {
            out << nl << "_" << p->name() << " = true;";
        }
        out << nl << "this." << name << " = " << name << ';';
        out << eb;

        //
        // Generate hasFoo and clearFoo for optional member.
        //
        if(optional)
        {
            out << sp;
            writeDocComment(out, dc);
            if(dc && dc->deprecated)
            {
                out << nl << "@Deprecated";
            }
            out << nl << "public boolean has" << capName << "()";
            out << sb;
            out << nl << "return _" << p->name() << ';';
            out << eb;

            out << sp;
            writeDocComment(out, dc);
            if(dc && dc->deprecated)
            {
                out << nl << "@Deprecated";
            }
            out << nl << "public void clear" << capName << "()";
            out << sb;
            out << nl << "_" << p->name() << " = false;";
            out << eb;

            const string optType =
                typeToString(type, TypeModeMember, getPackage(contained), metaData, true, true, local);

            out << sp;
            writeDocComment(out, dc);
            if(dc && dc->deprecated)
            {
                out << nl << "@Deprecated";
            }
            out << nl << "public void optional" << capName << '(' << optType << " v)";
            out << sb;
            out << nl << "if(v == null || !v.isPresent())";
            out << sb;
            out << nl << "_" << p->name() << " = false;";
            out << eb;
            out << nl << "else";
            out << sb;
            out << nl << "_" << p->name() << " = true;";
            if(b && b->kind() == Builtin::KindInt)
            {
                out << nl << name << " = v.getAsInt();";
            }
            else if(b && b->kind() == Builtin::KindLong)
            {
                out << nl << name << " = v.getAsLong();";
            }
            else if(b && b->kind() == Builtin::KindDouble)
            {
                out << nl << name << " = v.getAsDouble();";
            }
            else
            {
                out << nl << name << " = v.get();";
            }
            out << eb;
            out << eb;

            out << sp;
            writeDocComment(out, dc);
            if(dc && dc->deprecated)
            {
                out << nl << "@Deprecated";
            }
            out << nl << "public " << optType << " optional" << capName << "()";
            out << sb;
            out << nl << "if(_" << p->name() << ')';
            out << sb;
            if(classType)
            {
                out << nl << "return java.util.Optional.ofNullable(" << name << ");";
            }
            else if(b && b->kind() == Builtin::KindInt)
            {
                out << nl << "return java.util.OptionalInt.of(" << name << ");";
            }
            else if(b && b->kind() == Builtin::KindLong)
            {
                out << nl << "return java.util.OptionalLong.of(" << name << ");";
            }
            else if(b && b->kind() == Builtin::KindDouble)
            {
                out << nl << "return java.util.OptionalDouble.of(" << name << ");";
            }
            else
            {
                out << nl << "return java.util.Optional.of(" << name << ");";
            }
            out << eb;
            out << nl << "else";
            out << sb;
            if(b && b->kind() == Builtin::KindInt)
            {
                out << nl << "return java.util.OptionalInt.empty();";
            }
            else if(b && b->kind() == Builtin::KindLong)
            {
                out << nl << "return java.util.OptionalLong.empty();";
            }
            else if(b && b->kind() == Builtin::KindDouble)
            {
                out << nl << "return java.util.OptionalDouble.empty();";
            }
            else
            {
                out << nl << "return java.util.Optional.empty();";
            }
            out << eb;
            out << eb;
        }

        //
        // Check for bool type.
        //
        if(b && b->kind() == Builtin::KindBool)
        {
            if(cls && !validateMethod(ops, "is" + capName, 0, file, line))
            {
                return;
            }
            out << sp;
            if(dc && dc->deprecated)
            {
                out << nl << "@Deprecated";
            }
            out << nl << "public boolean is" << capName << "()";
            out << sb;
            if(optional)
            {
                out << nl << "if(!_" << p->name() << ')';
                out << sb;
                out << nl << "throw new java.util.NoSuchElementException(\"" << name << " is not set\");";
                out << eb;
            }
            out << nl << "return " << name << ';';
            out << eb;
        }

        //
        // Check for unmodified sequence type and emit indexing methods.
        //
        SequencePtr seq = SequencePtr::dynamicCast(type);
        if(seq)
        {
            if(!hasTypeMetaData(seq, metaData))
            {
                if(cls &&
                   (!validateMethod(ops, "get" + capName, 1, file, line) ||
                    !validateMethod(ops, "set" + capName, 2, file, line)))
                {
                    return;
                }

                string elem = typeToString(seq->type(), TypeModeMember, getPackage(contained), StringList(), true,
                                           false, local);

                //
                // Indexed getter.
                //
                out << sp;
                if(dc && dc->deprecated)
                {
                    out << nl << "@Deprecated";
                }
                out << nl << "public " << elem << " get" << capName << "(int index)";
                out << sb;
                if(optional)
                {
                    out << nl << "if(!_" << p->name() << ')';
                    out << sb;
                    out << nl << "throw new java.util.NoSuchElementException(\"" << name << " is not set\");";
                    out << eb;
                }
                out << nl << "return this." << name << "[index];";
                out << eb;

                //
                // Indexed setter.
                //
                out << sp;
                if(dc && dc->deprecated)
                {
                    out << nl << "@Deprecated";
                }
                out << nl << "public void set" << capName << "(int index, " << elem << " val)";
                out << sb;
                if(optional)
                {
                    out << nl << "if(!_" << p->name() << ')';
                    out << sb;
                    out << nl << "throw new java.util.NoSuchElementException(\"" << name << " is not set\");";
                    out << eb;
                }
                out << nl << "this." << name << "[index] = val;";
                out << eb;
            }
        }
    }
}

void
Slice::Gen::TypesVisitor::visitEnum(const EnumPtr& p)
{
    string name = fixKwd(p->name());
    string absolute = getAbsolute(p);
    EnumeratorList enumerators = p->getEnumerators();

    open(absolute, p->file());

    Output& out = output();

    out << sp;

    DocCommentPtr dc = parseDocComment(p);
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }

    out << nl << "public enum " << name;
    if(!p->isLocal())
    {
        out << " implements java.io.Serializable";
    }
    out << sb;

    for(EnumeratorList::const_iterator en = enumerators.begin(); en != enumerators.end(); ++en)
    {
        if(en != enumerators.begin())
        {
            out << ',';
        }
        DocCommentPtr edc = parseDocComment(*en);
        writeDocComment(out, edc);
        if(edc && edc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << fixKwd((*en)->name()) << '(' << (*en)->value() << ')';
    }
    out << ';';

    out << sp << nl << "public int value()";
    out << sb;
    out << nl << "return _value;";
    out << eb;

    out << sp << nl << "public static " << name << " valueOf(int v)";
    out << sb;
    out << nl << "switch(v)";
    out << sb;
    out.dec();
    for(EnumeratorList::const_iterator en = enumerators.begin(); en != enumerators.end(); ++en)
    {
        out << nl << "case " << (*en)->value() << ':';
        out.inc();
        out << nl << "return " << fixKwd((*en)->name()) << ';';
        out.dec();
    }
    out.inc();
    out << eb;
    out << nl << "return null;";
    out << eb;

    out << sp << nl << "private " << name << "(int v)";
    out << sb;
    out << nl << "_value = v;";
    out << eb;

    if(!p->isLocal())
    {
        out << sp << nl << "public void ice_write(com.zeroc.Ice.OutputStream ostr)";
        out << sb;
        out << nl << "ostr.writeEnum(_value, " << p->maxValue() << ");";
        out << eb;

        out << sp << nl << "public static void ice_write(com.zeroc.Ice.OutputStream ostr, " << name << " v)";
        out << sb;
        out << nl << "if(v == null)";
        out << sb;
        string firstEnum = fixKwd(enumerators.front()->name());
        out << nl << "ostr.writeEnum(" << absolute << '.' << firstEnum << ".value(), " << p->maxValue() << ");";
        out << eb;
        out << nl << "else";
        out << sb;
        out << nl << "ostr.writeEnum(v.value(), " << p->maxValue() << ");";
        out << eb;
        out << eb;

        out << sp << nl << "public static " << name << " ice_read(com.zeroc.Ice.InputStream istr)";
        out << sb;
        out << nl << "int v = istr.readEnum(" << p->maxValue() << ");";
        out << nl << "return validate(v);";
        out << eb;

        out << sp << nl << "private static " << name << " validate(int v)";
        out << sb;
        out << nl << "final " << name << " e = valueOf(v);";
        out << nl << "if(e == null)";
        out << sb;
        out << nl << "throw new com.zeroc.Ice.MarshalException(\"enumerator value \" + v + \" is out of range\");";
        out << eb;
        out << nl << "return e;";
        out << eb;
    }

    out << sp << nl << "private final int _value;";

    out << eb;
    close();
}

void
Slice::Gen::TypesVisitor::visitConst(const ConstPtr& p)
{
    string name = fixKwd(p->name());
    string package = getPackage(p);
    string absolute = getAbsolute(p);
    TypePtr type = p->type();

    open(absolute, p->file());

    Output& out = output();

    out << sp;

    DocCommentPtr dc = parseDocComment(p);
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }

    out << nl << "public interface " << name;
    out << sb;
    out << nl << typeToString(type, TypeModeIn, package) << " value = ";
    writeConstantValue(out, type, p->valueType(), p->value(), package);
    out << ';' << eb;
    close();
}

bool
Slice::Gen::TypesVisitor::validateMethod(const OperationList& ops, const std::string& name, int numArgs,
                                         const string& file, const string& line)
{
    for(OperationList::const_iterator i = ops.begin(); i != ops.end(); ++i)
    {
        if((*i)->name() == name)
        {
            int numParams = static_cast<int>((*i)->parameters().size());
            if(numArgs >= numParams && numArgs - numParams <= 1)
            {
                ostringstream ostr;
                ostr << "operation `" << name << "' conflicts with method for data member";
                emitError(file, line, ostr.str());
                return false;
            }
            break;
        }
    }
    return true;
}

Slice::Gen::CompactIdVisitor::CompactIdVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

bool
Slice::Gen::CompactIdVisitor::visitClassDefStart(const ClassDefPtr& p)
{
    string prefix = getPackagePrefix(p);
    if(!prefix.empty())
    {
        prefix = prefix + ".";
    }
    if(p->compactId() >= 0)
    {
        ostringstream os;
        os << prefix << "com.zeroc.IceCompactId.TypeId_" << p->compactId();
        open(os.str(), p->file());

        Output& out = output();
        out << sp << nl << "public class TypeId_" << p->compactId();
        out << sb;
        out << nl << "public final static String typeId = \"" << p->scoped() << "\";";
        out << eb;

        close();
    }
    return false;
}

Slice::Gen::HelperVisitor::HelperVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

void
Slice::Gen::HelperVisitor::visitSequence(const SequencePtr& p)
{
    //
    // Don't generate helper for a sequence of a local type.
    //
    if(p->isLocal())
    {
        return;
    }

    BuiltinPtr builtin = BuiltinPtr::dynamicCast(p->type());
    if(builtin &&
       (builtin->kind() == Builtin::KindByte || builtin->kind() == Builtin::KindShort ||
        builtin->kind() == Builtin::KindInt || builtin->kind() == Builtin::KindLong ||
        builtin->kind() == Builtin::KindFloat || builtin->kind() == Builtin::KindDouble))
    {
        string prefix = "java:buffer";
        string meta;
        if(p->findMetaData(prefix, meta))
        {
            return; // No holders for buffer types.
        }
    }

    string name = p->name();
    string absolute = getAbsolute(p);
    string helper = getAbsolute(p, "", "", "Helper");
    string package = getPackage(p);
    string typeS = typeToString(p, TypeModeIn, package);

    //
    // We cannot allocate an array of a generic type, such as
    //
    // arr = new Map<String, String>[sz];
    //
    // Attempting to compile this code results in a "generic array creation" error
    // message. This problem can occur when the sequence's element type is a
    // dictionary, or when the element type is a nested sequence that uses a custom
    // mapping.
    //
    // The solution is to rewrite the code as follows:
    //
    // arr = (Map<String, String>[])new Map[sz];
    //
    // Unfortunately, this produces an unchecked warning during compilation, so we
    // annotate the read() method to suppress the warning.
    //
    // A simple test is to look for a "<" character in the content type, which
    // indicates the use of a generic type.
    //
    bool suppressUnchecked = false;

    string instanceType, formalType;
    bool customType = getSequenceTypes(p, "", StringList(), instanceType, formalType, false);

    if(!customType)
    {
        //
        // Determine sequence depth.
        //
        int depth = 0;
        TypePtr origContent = p->type();
        SequencePtr s = SequencePtr::dynamicCast(origContent);
        while(s)
        {
            //
            // Stop if the inner sequence type has a custom, serializable or protobuf type.
            //
            if(hasTypeMetaData(s))
            {
                break;
            }
            depth++;
            origContent = s->type();
            s = SequencePtr::dynamicCast(origContent);
        }

        string origContentS = typeToString(origContent, TypeModeIn, package);
        suppressUnchecked = origContentS.find('<') != string::npos;
    }

    open(helper, p->file());
    Output& out = output();

    int iter;

    out << sp << nl << "public final class " << name << "Helper";
    out << sb;

    out << nl << "public static void write(com.zeroc.Ice.OutputStream ostr, " << typeS << " v)";
    out << sb;
    iter = 0;
    writeSequenceMarshalUnmarshalCode(out, package, p, "v", true, iter, false);
    out << eb;

    out << sp;
    if(suppressUnchecked)
    {
        out << nl << "@SuppressWarnings(\"unchecked\")";
    }
    out << nl << "public static " << typeS << " read(com.zeroc.Ice.InputStream istr)";
    out << sb;
    out << nl << typeS << " v;";
    iter = 0;
    writeSequenceMarshalUnmarshalCode(out, package, p, "v", false, iter, false);
    out << nl << "return v;";
    out << eb;

    out << eb;
    close();
}

void
Slice::Gen::HelperVisitor::visitDictionary(const DictionaryPtr& p)
{
    //
    // Don't generate helper for a dictionary containing a local type
    //
    if(p->isLocal())
    {
        return;
    }

    TypePtr key = p->keyType();
    TypePtr value = p->valueType();

    string name = p->name();
    string absolute = getAbsolute(p);
    string helper = getAbsolute(p, "", "", "Helper");
    string package = getPackage(p);
    StringList metaData = p->getMetaData();
    string formalType = typeToString(p, TypeModeIn, package, StringList(), true);

    open(helper, p->file());
    Output& out = output();

    int iter;

    out << sp << nl << "public final class " << name << "Helper";
    out << sb;

    out << nl << "public static void write(com.zeroc.Ice.OutputStream ostr, " << formalType << " v)";
    out << sb;
    iter = 0;
    writeDictionaryMarshalUnmarshalCode(out, package, p, "v", true, iter, false);
    out << eb;

    out << sp << nl << "public static " << formalType << " read(com.zeroc.Ice.InputStream istr)";
    out << sb;
    out << nl << formalType << " v;";
    iter = 0;
    writeDictionaryMarshalUnmarshalCode(out, package, p, "v", false, iter, false);
    out << nl << "return v;";
    out << eb;

    out << eb;
    close();
}

Slice::Gen::ProxyVisitor::ProxyVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

bool
Slice::Gen::ProxyVisitor::visitClassDefStart(const ClassDefPtr& p)
{
    if(p->isLocal())
    {
        return false;
    }

    //
    // Don't generate a proxy interface for a class with no operations.
    //
    const OperationList ops = p->allOperations();
    if(!p->isInterface() && ops.empty())
    {
        return false;
    }

    string name = p->name();
    ClassList bases = p->bases();
    string package = getPackage(p);
    string absolute = getAbsolute(p, "", "", "Prx");

    open(absolute, p->file());

    Output& out = output();

    //
    // For proxy purposes, we can ignore a base class if it has no operations.
    //
    if(!bases.empty() && !bases.front()->isInterface() && bases.front()->allOperations().empty())
    {
        bases.pop_front();
    }

    DocCommentPtr dc = parseDocComment(p);

    //
    // Generate a Java interface as the user-visible type
    //
    out << sp;
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    out << nl << "public interface " << name << "Prx extends ";
    out.useCurrentPosAsIndent();
    if(bases.empty())
    {
        out << "com.zeroc.Ice.ObjectPrx";
    }
    else
    {
        for(ClassList::const_iterator q = bases.begin(); q != bases.end(); ++q)
        {
            if(q != bases.begin())
            {
                out << ',' << nl;
            }
            out << getAbsolute(*q, package, "", "Prx");
        }
    }
    out.restoreIndent();

    out << sb;

    return true;
}

void
Slice::Gen::ProxyVisitor::visitClassDefEnd(const ClassDefPtr& p)
{
    Output& out = output();

    DocCommentPtr dc = parseDocComment(p);

    const string contextParam = "java.util.Map<String, String> context";

    out << sp;
    writeDocComment(out,
                    "Contacts the remote server to verify that the object implements this type.\n"
                    "Raises a local exception if a communication error occurs.\n"
                    "@param obj The untyped proxy.\n"
                    "@return A proxy for this type, or null if the object does not support this type.");
    out << nl << "static " << p->name() << "Prx checkedCast(com.zeroc.Ice.ObjectPrx obj)";
    out << sb;
    out << nl << "return com.zeroc.Ice.ObjectPrx._checkedCast(obj, ice_staticId(), " << p->name()
        << "Prx.class, _" << p->name() << "PrxI.class);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Contacts the remote server to verify that the object implements this type.\n"
                    "Raises a local exception if a communication error occurs.\n"
                    "@param obj The untyped proxy.\n"
                    "@param context The Context map to send with the invocation.\n"
                    "@return A proxy for this type, or null if the object does not support this type.");
    out << nl << "static " << p->name() << "Prx checkedCast(com.zeroc.Ice.ObjectPrx obj, " << contextParam << ')';
    out << sb;
    out << nl << "return com.zeroc.Ice.ObjectPrx._checkedCast(obj, context, ice_staticId(), " << p->name()
        << "Prx.class, _" << p->name() << "PrxI.class);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Contacts the remote server to verify that a facet of the object implements this type.\n"
                    "Raises a local exception if a communication error occurs.\n"
                    "@param obj The untyped proxy.\n"
                    "@param facet The name of the desired facet.\n"
                    "@return A proxy for this type, or null if the object does not support this type.");
    out << nl << "static " << p->name() << "Prx checkedCast(com.zeroc.Ice.ObjectPrx obj, String facet)";
    out << sb;
    out << nl << "return com.zeroc.Ice.ObjectPrx._checkedCast(obj, facet, ice_staticId(), " << p->name()
        << "Prx.class, _" << p->name() << "PrxI.class);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Contacts the remote server to verify that a facet of the object implements this type.\n"
                    "Raises a local exception if a communication error occurs.\n"
                    "@param obj The untyped proxy.\n"
                    "@param facet The name of the desired facet.\n"
                    "@param context The Context map to send with the invocation.\n"
                    "@return A proxy for this type, or null if the object does not support this type.");
    out << nl << "static " << p->name() << "Prx checkedCast(com.zeroc.Ice.ObjectPrx obj, String facet, "
        << contextParam << ')';
    out << sb;
    out << nl << "return com.zeroc.Ice.ObjectPrx._checkedCast(obj, facet, context, ice_staticId(), " << p->name()
        << "Prx.class, _" << p->name() << "PrxI.class);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Downcasts the given proxy to this type without contacting the remote server.\n"
                    "@param obj The untyped proxy.\n"
                    "@return A proxy for this type.");
    out << nl << "static " << p->name() << "Prx uncheckedCast(com.zeroc.Ice.ObjectPrx obj)";
    out << sb;
    out << nl << "return com.zeroc.Ice.ObjectPrx._uncheckedCast(obj, " << p->name() << "Prx.class, _"
        << p->name() << "PrxI.class);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Downcasts the given proxy to this type without contacting the remote server.\n"
                    "@param obj The untyped proxy.\n"
                    "@param facet The name of the desired facet.\n"
                    "@return A proxy for this type.");
    out << nl << "static " << p->name() << "Prx uncheckedCast(com.zeroc.Ice.ObjectPrx obj, String facet)";
    out << sb;
    out << nl << "return com.zeroc.Ice.ObjectPrx._uncheckedCast(obj, facet, " << p->name() << "Prx.class, _"
        << p->name() << "PrxI.class);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the per-proxy context.\n"
                    "@param newContext The context for the new proxy.\n"
                    "@return A proxy with the specified per-proxy context.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_context(java.util.Map<String, String> newContext)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_context(newContext);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the adapter ID.\n"
                    "@param newAdapterId The adapter ID for the new proxy.\n"
                    "@return A proxy with the specified adapter ID.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_adapterId(String newAdapterId)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_adapterId(newAdapterId);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the endpoints.\n"
                    "@param newEndpoints The endpoints for the new proxy.\n"
                    "@return A proxy with the specified endpoints.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_endpoints(com.zeroc.Ice.Endpoint[] newEndpoints)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_endpoints(newEndpoints);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the locator cache timeout.\n"
                    "@param newTimeout The new locator cache timeout (in seconds).\n"
                    "@return A proxy with the specified locator cache timeout.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_locatorCacheTimeout(int newTimeout)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_locatorCacheTimeout(newTimeout);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the invocation timeout.\n"
                    "@param newTimeout The new invocation timeout (in seconds).\n"
                    "@return A proxy with the specified invocation timeout.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_invocationTimeout(int newTimeout)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_invocationTimeout(newTimeout);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for connection caching.\n"
                    "@param newCache <code>true</code> if the new proxy should cache connections; <code>false</code> otherwise.\n"
                    "@return A proxy with the specified caching policy.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_connectionCached(boolean newCache)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_connectionCached(newCache);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the endpoint selection policy.\n"
                    "@param newType The new endpoint selection policy.\n"
                    "@return A proxy with the specified endpoint selection policy.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_endpointSelection(com.zeroc.Ice.EndpointSelectionType newType)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_endpointSelection(newType);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for how it selects endpoints.\n"
                    "@param b If <code>b</code> is <code>true</code>, only endpoints that use a secure transport are\n"
                    "used by the new proxy. If <code>b</code> is false, the returned proxy uses both secure and\n"
                    "insecure endpoints.\n"
                    "@return A proxy with the specified selection policy.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_secure(boolean b)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_secure(b);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the encoding used to marshal parameters.\n"
                    "@param e The encoding version to use to marshal request parameters.\n"
                    "@return A proxy with the specified encoding version.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_encodingVersion(com.zeroc.Ice.EncodingVersion e)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_encodingVersion(e);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for its endpoint selection policy.\n"
                    "@param b If <code>b</code> is <code>true</code>, the new proxy will use secure endpoints for invocations\n"
                    "and only use insecure endpoints if an invocation cannot be made via secure endpoints. If <code>b</code> is\n"
                    "<code>false</code>, the proxy prefers insecure endpoints to secure ones.\n"
                    "@return A proxy with the specified selection policy.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_preferSecure(boolean b)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_preferSecure(b);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the router.\n"
                    "@param router The router for the new proxy.\n"
                    "@return A proxy with the specified router.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_router(com.zeroc.Ice.RouterPrx router)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_router(router);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for the locator.\n"
                    "@param locator The locator for the new proxy.\n"
                    "@return A proxy with the specified locator.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_locator(com.zeroc.Ice.LocatorPrx locator)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_locator(locator);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for collocation optimization.\n"
                    "@param b <code>true</code> if the new proxy enables collocation optimization; <code>false</code> otherwise.\n"
                    "@return A proxy with the specified collocation optimization.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_collocationOptimized(boolean b)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_collocationOptimized(b);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, but uses twoway invocations.\n"
                    "@return A proxy that uses twoway invocations.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_twoway()";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_twoway();";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, but uses oneway invocations.\n"
                    "@return A proxy that uses oneway invocations.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_oneway()";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_oneway();";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, but uses batch oneway invocations.\n"
                    "@return A proxy that uses batch oneway invocations.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_batchOneway()";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_batchOneway();";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, but uses datagram invocations.\n"
                    "@return A proxy that uses datagram invocations.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_datagram()";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_datagram();";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, but uses batch datagram invocations.\n"
                    "@return A proxy that uses batch datagram invocations.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_batchDatagram()";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_batchDatagram();";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for compression.\n"
                    "@param co <code>true</code> enables compression for the new proxy; <code>false</code> disables compression.\n"
                    "@return A proxy with the specified compression setting.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_compress(boolean co)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_compress(co);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for its connection timeout setting.\n"
                    "@param t The connection timeout for the proxy in milliseconds.\n"
                    "@return A proxy with the specified timeout.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_timeout(int t)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_timeout(t);";
    out << eb;

    out << sp;
    writeDocComment(out,
                    "Returns a proxy that is identical to this proxy, except for its connection ID.\n"
                    "@param connectionId The connection ID for the new proxy. An empty string removes the connection ID.\n"
                    "@return A proxy with the specified connection ID.");
    out << nl << "@Override";
    out << nl << "default " << p->name() << "Prx ice_connectionId(String connectionId)";
    out << sb;
    out << nl << "return (" << p->name() << "Prx)_ice_connectionId(connectionId);";
    out << eb;

    out << sp;
    out << nl << "static String ice_staticId()";
    out << sb;
    out << nl << "return \"" << p->scoped() << "\";";
    out << eb;

    out << eb;
    close();

    string absolute = getAbsolute(p, "", "_", "PrxI");

    open(absolute, p->file());

    Output& outi = output();

    outi << sp;
    if(dc && dc->deprecated)
    {
        outi << nl << "@Deprecated";
    }
    outi << nl << "public class _" << p->name() << "PrxI extends com.zeroc.Ice._ObjectPrxI implements " << p->name()
         << "Prx";
    outi << sb;
    outi << sp << nl << "public static final long serialVersionUID = 0L;";
    outi << eb;
    close();
}

void
Slice::Gen::ProxyVisitor::visitOperation(const OperationPtr& p)
{
    const string name = fixKwd(p->name());
    const ContainerPtr container = p->container();
    const ClassDefPtr cl = ClassDefPtr::dynamicCast(container);
    const string package = getPackage(cl);

    Output& out = output();

    const TypePtr ret = p->returnType();
    const string retS = getResultType(p, package, false, false);
    const bool returnsParams = ret || !p->outParameters().empty();
    const vector<string> params = getParamsProxy(p, package, false);
    const bool sendsOptionals = p->sendsOptionals();
    vector<string> paramsOpt;
    if(sendsOptionals)
    {
        paramsOpt = getParamsProxy(p, package, true);
    }
    const vector<string> args = getInArgs(p);

    ExceptionList throws = p->throws();
    throws.sort();
    throws.unique();

    //
    // Arrange exceptions into most-derived to least-derived order. If we don't
    // do this, a base exception handler can appear before a derived exception
    // handler, causing compiler warnings and resulting in the base exception
    // being marshaled instead of the derived exception.
    //
#if defined(__SUNPRO_CC)
    throws.sort(Slice::derivedToBaseCompare);
#else
    throws.sort(Slice::DerivedToBaseCompare());
#endif

    const string contextParamName = getEscapedParamName(p, "context");
    const string contextDoc = "@param " + contextParamName +" The Context map to send with the invocation.";
    const string contextParam = "java.util.Map<String, String> " + contextParamName;
    const string noExplicitContextArg = "com.zeroc.Ice.ObjectPrx.noExplicitContext";

    DocCommentPtr dc = parseDocComment(p);

    //
    // Synchronous methods with required parameters.
    //
    out << sp;
    writeProxyDocComment(out, p, package, dc, false, false);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    out << nl << "default " << retS << ' ' << name << spar << params << epar;
    writeThrowsClause(package, throws);
    out << sb;
    out << nl;
    if(returnsParams)
    {
        out << "return ";
    }
    out << name << spar << args << noExplicitContextArg << epar << ';';
    out << eb;

    out << sp;
    writeProxyDocComment(out, p, package, dc, false, true);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    out << nl << "default " << retS << ' ' << name << spar << params << contextParam << epar;
    writeThrowsClause(package, throws);
    out << sb;
    if(throws.empty())
    {
        out << nl;
        if(returnsParams)
        {
            out << "return ";
        }
        out << "_iceI_" << p->name() << "Async" << spar << args << contextParamName << "true" << epar << ".waitForResponse();";
    }
    else
    {
        out << nl << "try";
        out << sb;
        out << nl;
        if(returnsParams)
        {
            out << "return ";
        }
        out << "_iceI_" << p->name() << "Async" << spar << args << contextParamName << "true" << epar << ".waitForResponseOrUserEx();";
        out << eb;
        for(ExceptionList::const_iterator t = throws.begin(); t != throws.end(); ++t)
        {
            string exS = getAbsolute(*t, package);
            out << nl << "catch(" << exS << " ex)";
            out << sb;
            out << nl << "throw ex;";
            out << eb;
        }
        out << nl << "catch(com.zeroc.Ice.UserException ex)";
        out << sb;
        out << nl << "throw new com.zeroc.Ice.UnknownUserException(ex.ice_id(), ex);";
        out << eb;
    }
    out << eb;

    //
    // Synchronous methods using optional parameters (if any).
    //
    if(sendsOptionals)
    {
        out << sp;
        writeProxyDocComment(out, p, package, dc, false, false);
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "default " << retS << ' ' << name << spar << paramsOpt << epar;
        writeThrowsClause(package, throws);
        out << sb;
        out << nl;
        if(returnsParams)
        {
            out << "return ";
        }
        out << name << spar << args << noExplicitContextArg << epar << ';';
        out << eb;

        out << sp;
        writeProxyDocComment(out, p, package, dc, false, true);
        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "default " << retS << ' ' << name << spar << paramsOpt << contextParam << epar;
        writeThrowsClause(package, throws);
        out << sb;
        if(throws.empty())
        {
            out << nl;
            if(returnsParams)
            {
                out << "return ";
            }
            out << "_iceI_" << p->name() << "Async" << spar << args << contextParamName << "true" << epar << ".waitForResponse();";
        }
        else
        {
            out << nl << "try";
            out << sb;
            out << nl;
            if(returnsParams)
            {
                out << "return ";
            }
            out << "_iceI_" << p->name() << "Async" << spar << args << contextParamName << "true" << epar << ".waitForResponseOrUserEx();";
            out << eb;
            for(ExceptionList::const_iterator t = throws.begin(); t != throws.end(); ++t)
            {
                string exS = getAbsolute(*t, package);
                out << nl << "catch(" << exS << " ex)";
                out << sb;
                out << nl << "throw ex;";
                out << eb;
            }
            out << nl << "catch(com.zeroc.Ice.UserException ex)";
            out << sb;
            out << nl << "throw new com.zeroc.Ice.UnknownUserException(ex.ice_id(), ex);";
            out << eb;
        }
        out << eb;
    }

    //
    // Asynchronous methods with required parameters.
    //
    out << sp;
    writeProxyDocComment(out, p, package, dc, true, false);

    const string future = getFutureType(p, package);

    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    out << nl << "default " << future << ' ' << p->name() << "Async" << spar << params << epar;
    out << sb;
    out << nl << "return _iceI_" << p->name() << "Async" << spar << args << noExplicitContextArg << "false" << epar << ';';
    out << eb;

    out << sp;
    writeProxyDocComment(out, p, package, dc, true, true);

    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    out << nl << "default " << future << ' ' << p->name() << "Async" << spar << params << contextParam << epar;
    out << sb;
    out << nl << "return _iceI_" << p->name() << "Async" << spar << args << contextParamName << "false" << epar << ';';
    out << eb;

    const string futureImpl = getFutureImplType(p, package);

    out << sp;
    out << nl << "default " << futureImpl << " _iceI_" << p->name() << "Async" << spar << getParamsProxy(p, package, false, true)
        << "java.util.Map<String, String> context"
        << "boolean sync" << epar;
    out << sb;
    out << nl << futureImpl << " f = new com.zeroc.IceInternal.OutgoingAsync<>(this, \"" << p->name() << "\", "
        << sliceModeToIceMode(p->sendMode()) << ", sync, "
        << (throws.empty() ? "null" : "_iceE_" + p->name()) << ");";

    out << nl << "f.invoke(";
    out.useCurrentPosAsIndent();
    out << (p->returnsData() ? "true" : "false") << ", context, " << opFormatTypeToString(p)
        << ", ";
    if(!p->inParameters().empty())
    {
        out << "ostr -> {";
        out.inc();
        writeMarshalProxyParams(out, package, p, false);
        out.dec();
        out << nl << '}';
    }
    else
    {
        out << "null";
    }
    out << ", ";
    if(returnsParams)
    {
        out << "istr -> {";
        out.inc();
        writeUnmarshalProxyResults(out, package, p);
        out.dec();
        out << nl << "}";
    }
    else
    {
        out << "null";
    }
    out.restoreIndent();
    out << ");";
    out << nl << "return f;";
    out << eb;

    if(!throws.empty())
    {
        out << sp << nl << "static final Class<?>[] _iceE_" << p->name() << " =";
        out << sb;
        for(ExceptionList::const_iterator t = throws.begin(); t != throws.end(); ++t)
        {
            if(t != throws.begin())
            {
                out << ",";
            }
            out << nl << getAbsolute(*t, package) << ".class";
        }
        out << eb << ';';
    }

    if(sendsOptionals)
    {
        out << sp;
        writeProxyDocComment(out, p, package, dc, true, false);

        const string future = getFutureType(p, package);

        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "default " << future << ' ' << p->name() << "Async" << spar << paramsOpt << epar;
        out << sb;
        out << nl << "return _iceI_" << p->name() << "Async" << spar << args << noExplicitContextArg << "false" << epar
            << ';';
        out << eb;

        out << sp;
        writeProxyDocComment(out, p, package, dc, true, true);

        if(dc && dc->deprecated)
        {
            out << nl << "@Deprecated";
        }
        out << nl << "default " << future << ' ' << p->name() << "Async" << spar << paramsOpt << contextParam << epar;
        out << sb;
        out << nl << "return _iceI_" << p->name() << "Async" << spar << args << contextParamName << "false" << epar << ';';
        out << eb;

        out << sp;
        out << nl << "default " << futureImpl << " _iceI_" << p->name() << "Async" << spar << getParamsProxy(p, package, true, true)
            << "java.util.Map<String, String> context"
            << "boolean sync" << epar;
        out << sb;
        out << nl << futureImpl << " f = new com.zeroc.IceInternal.OutgoingAsync<>(this, \"" << p->name() << "\", "
            << sliceModeToIceMode(p->sendMode()) << ", sync, "
            << (throws.empty() ? "null" : "_iceE_" + p->name()) << ");";

        out << nl << "f.invoke(";
        out.useCurrentPosAsIndent();
        out << (p->returnsData() ? "true" : "false") << ", context, " << opFormatTypeToString(p) << ", ";
        if(!p->inParameters().empty())
        {
            out << "ostr -> {";
            out.inc();
            writeMarshalProxyParams(out, package, p, true);
            out.dec();
            out << nl << '}';
        }
        else
        {
            out << "null";
        }
        out << ", ";
        if(returnsParams)
        {
            out << "istr -> {";
            out.inc();
            writeUnmarshalProxyResults(out, package, p);
            out.dec();
            out << nl << "}";
        }
        else
        {
            out << "null";
        }
        out.restoreIndent();
        out << ");";
        out << nl << "return f;";
        out << eb;
    }
}

Slice::Gen::DispatcherVisitor::DispatcherVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

bool
Slice::Gen::DispatcherVisitor::visitClassDefStart(const ClassDefPtr& p)
{
    if(p->isLocal() || p->isInterface() || p->allOperations().empty())
    {
        return false;
    }

    const string name = p->name();
    const string absolute = getAbsolute(p, "", "_", "Disp");
    const string package = getPackage(p);

    open(absolute, p->file());

    Output& out = output();

    out << sp;
    DocCommentPtr dc = parseDocComment(p);
    writeDocComment(out, dc);
    if(dc && dc->deprecated)
    {
        out << nl << "@Deprecated";
    }
    out << nl << "public interface _" << name << "Disp";

    //
    // For dispatch purposes, we can ignore a base class if it has no operations.
    //
    ClassList bases = p->bases();
    if(!bases.empty() && !bases.front()->isInterface() && bases.front()->allOperations().empty())
    {
        bases.pop_front();
    }

    if(bases.empty())
    {
        out << " extends com.zeroc.Ice.Object";
    }
    else
    {
        out << " extends ";
        out.useCurrentPosAsIndent();
        for(ClassList::const_iterator q = bases.begin(); q != bases.end(); ++q)
        {
            if(q != bases.begin())
            {
                out << ',' << nl;
            }
            if(!(*q)->isInterface())
            {
                out << getAbsolute(*q, package, "_", "Disp");
            }
            else
            {
                out << getAbsolute(*q, package);
            }
        }
        out.restoreIndent();
    }
    out << sb;

    writeDispatch(out, p);

    out << eb;
    close();

    return false;
}

Slice::Gen::ImplVisitor::ImplVisitor(const string& dir) :
    JavaVisitor(dir)
{
}

bool
Slice::Gen::ImplVisitor::visitClassDefStart(const ClassDefPtr& p)
{
    if(!p->isAbstract())
    {
        return false;
    }

    string name = p->name();
    ClassList bases = p->bases();
    string package = getPackage(p);
    string absolute = getAbsolute(p, "", "", "I");

    open(absolute, p->file());

    Output& out = output();

    out << sp << nl << "public final class " << name << 'I';
    if(p->isInterface())
    {
        out << " implements " << fixKwd(name);
    }
    else
    {
        if(p->isLocal())
        {
            out << " extends " << fixKwd(name);
        }
        else
        {
            out << " implements _" << name << "Disp";
        }
    }
    out << sb;

    out << nl << "public " << name << "I()";
    out << sb;
    out << eb;

    OperationList ops = p->allOperations();
    for(OperationList::iterator r = ops.begin(); r != ops.end(); ++r)
    {
        writeOperation(out, package, *r, p->isLocal());
    }

    out << eb;
    close();

    return false;
}

string
Slice::Gen::ImplVisitor::getDefaultValue(const string& package, const TypePtr& type, bool optional)
{
    const BuiltinPtr b = BuiltinPtr::dynamicCast(type);
    if(optional)
    {
        if(b && b->kind() == Builtin::KindDouble)
        {
            return "java.util.OptionalDouble.empty()";
        }
        else if(b && b->kind() == Builtin::KindInt)
        {
            return "java.util.OptionalInt.empty()";
        }
        else if(b && b->kind() == Builtin::KindLong)
        {
            return "java.util.OptionalLong.empty()";
        }
        else
        {
            return "java.util.Optional.empty()";
        }
    }
    else
    {
        if(b)
        {
            switch(b->kind())
            {
                case Builtin::KindBool:
                {
                    return "false";
                    break;
                }
                case Builtin::KindByte:
                {
                    return "(byte)0";
                    break;
                }
                case Builtin::KindShort:
                {
                    return "(short)0";
                    break;
                }
                case Builtin::KindInt:
                case Builtin::KindLong:
                {
                    return "0";
                    break;
                }
                case Builtin::KindFloat:
                {
                    return "(float)0.0";
                    break;
                }
                case Builtin::KindDouble:
                {
                    return "0.0";
                    break;
                }
                case Builtin::KindString:
                {
                    return "\"\"";
                    break;
                }
                case Builtin::KindObject:
                case Builtin::KindObjectProxy:
                case Builtin::KindLocalObject:
                case Builtin::KindValue:
                {
                    return "null";
                    break;
                }
            }
        }
        else
        {
            EnumPtr en = EnumPtr::dynamicCast(type);
            if(en)
            {
                EnumeratorList enumerators = en->getEnumerators();
                return getAbsolute(en, package) + '.' + fixKwd(enumerators.front()->name());
            }
        }
    }

    return "null";
}

bool
Slice::Gen::ImplVisitor::initResult(Output& out, const string& package, const OperationPtr& op)
{
    const string retS = getResultType(op, package, false, true);

    if(op->hasMarshaledResult())
    {
        out << nl << retS << " r = new " << retS << spar;
        const ParamDeclList outParams = op->outParameters();
        if(op->returnType())
        {
            out << getDefaultValue(package, op->returnType(), op->returnIsOptional());
        }
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            out << getDefaultValue(package, (*p)->type(), (*p)->optional());
        }
        out << "current" << epar << ';';
    }
    else if(op->returnsMultipleValues())
    {
        out << nl << retS << " r = new " << retS << "();";
        string retval = "returnValue";
        const ParamDeclList outParams = op->outParameters();
        for(ParamDeclList::const_iterator p = outParams.begin(); p != outParams.end(); ++p)
        {
            out << nl << "r." << fixKwd((*p)->name()) << " = "
                << getDefaultValue(package, (*p)->type(), (*p)->optional()) << ';';
            if((*p)->name() == "returnValue")
            {
                retval = "_returnValue";
            }
        }
        if(op->returnType())
        {
            out << nl << "r." << retval << " = "
                << getDefaultValue(package, op->returnType(), op->returnIsOptional()) << ';';
        }
    }
    else
    {
        TypePtr type = op->returnType();
        bool optional = op->returnIsOptional();
        if(!type)
        {
            const ParamDeclList outParams = op->outParameters();
            if(!outParams.empty())
            {
                assert(outParams.size() == 1);
                type = outParams.front()->type();
                optional = outParams.front()->optional();
            }
        }
        if(type)
        {
            out << nl << retS << " r = " << getDefaultValue(package, type, optional) << ';';
        }
        else
        {
            return false;
        }
    }

    return true;
}

void
Slice::Gen::ImplVisitor::writeOperation(Output& out, const string& package, const OperationPtr& op, bool local)
{
    string opName = op->name();

    ExceptionList throws = op->throws();
    throws.sort();
    throws.unique();

    const ContainerPtr container = op->container();
    const ClassDefPtr cl = ClassDefPtr::dynamicCast(container);
    const vector<string> params = getParams(op, package);
    const string currentParam = "com.zeroc.Ice.Current " + getEscapedParamName(op, "current");

    if(local)
    {
        out << sp;
        out << nl << "@Override";
        out << nl << "public " << getResultType(op, package, false, false) << ' ' << fixKwd(opName) << spar << params
            << epar;
        writeThrowsClause(package, throws, op);

        out << sb;
        if(initResult(out, package, op))
        {
            out << nl << "return r;";
        }
        out << eb;

        if(cl->hasMetaData("async-oneway") || op->hasMetaData("async-oneway"))
        {
            out << sp;
            out << nl << "@Override";
            out << nl << getFutureType(op, package) << ' ' << opName << "Async" << spar << params << epar;
            out << sb;
            out << nl << "return null;";
            out << eb;
        }
    }
    else
    {
        const bool amd = cl->hasMetaData("amd") || op->hasMetaData("amd");

        if(amd)
        {
            const string retS = getResultType(op, package, true, true);
            out << sp;
            out << nl << "@Override";
            out << nl << "public java.util.concurrent.CompletionStage<" << retS << "> " << opName << "Async" << spar
                << params << currentParam << epar;
            writeThrowsClause(package, throws, op);
            out << sb;
            if(initResult(out, package, op))
            {
                out << nl << "return java.util.concurrent.CompletableFuture.completedFuture(r);";
            }
            else
            {
                out << nl << "return java.util.concurrent.CompletableFuture.completedFuture((Void)null);";
            }
            out << eb;
        }
        else
        {
            out << sp;
            out << nl << "@Override";
            out << nl << "public " << getResultType(op, package, false, true) << ' ' << fixKwd(opName) << spar << params
                << currentParam << epar;
            writeThrowsClause(package, throws, op);
            out << sb;
            if(initResult(out, package, op))
            {
                out << nl << "return r;";
            }
            out << eb;
        }
    }
}
