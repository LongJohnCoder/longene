/*
 * IDL Compiler
 *
 * Copyright 2002 Ove Kaaven
 * Copyright 2004 Mike McCormack
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <string.h>
#include <ctype.h>

#include "widl.h"
#include "utils.h"
#include "parser.h"
#include "header.h"
#include "typegen.h"
#include "expr.h"

#define END_OF_LIST(list)       \
  do {                          \
    if (list) {                 \
      while (NEXT_LINK(list))   \
        list = NEXT_LINK(list); \
    }                           \
  } while(0)

static FILE* proxy;
static int indent = 0;

/* FIXME: support generation of stubless proxies */

static void print_proxy( const char *format, ... )
{
  va_list va;
  va_start( va, format );
  print( proxy, indent, format, va );
  va_end( va );
}

static void write_stubdescproto(void)
{
  print_proxy( "static const MIDL_STUB_DESC Object_StubDesc;\n");
  print_proxy( "\n");
}

static void write_stubdesc(int expr_eval_routines)
{
  print_proxy( "static const MIDL_STUB_DESC Object_StubDesc =\n{\n");
  indent++;
  print_proxy( "0,\n");
  print_proxy( "NdrOleAllocate,\n");
  print_proxy( "NdrOleFree,\n");
  print_proxy( "{0}, 0, 0, %s, 0,\n", expr_eval_routines ? "ExprEvalRoutines" : "0");
  print_proxy( "__MIDL_TypeFormatString.Format,\n");
  print_proxy( "1, /* -error bounds_check flag */\n");
  print_proxy( "0x10001, /* Ndr library version */\n");
  print_proxy( "0,\n");
  print_proxy( "0x50100a4, /* MIDL Version 5.1.164 */\n");
  print_proxy( "0,\n");
  print_proxy("%s,\n", list_empty(&user_type_list) ? "0" : "UserMarshalRoutines");
  print_proxy( "0,  /* notify & notify_flag routine table */\n");
  print_proxy( "1,  /* Flags */\n");
  print_proxy( "0,  /* Reserved3 */\n");
  print_proxy( "0,  /* Reserved4 */\n");
  print_proxy( "0   /* Reserved5 */\n");
  indent--;
  print_proxy( "};\n");
  print_proxy( "\n");
}

static void init_proxy(const statement_list_t *stmts)
{
  if (proxy) return;
  if(!(proxy = fopen(proxy_name, "w")))
    error("Could not open %s for output\n", proxy_name);
  print_proxy( "/*** Autogenerated by WIDL %s from %s - Do not edit ***/\n", PACKAGE_VERSION, input_name);
  print_proxy( "\n");
  print_proxy( "#ifndef __REDQ_RPCPROXY_H_VERSION__\n");
  print_proxy( "#define __REQUIRED_RPCPROXY_H_VERSION__ 440\n");
  print_proxy( "#endif /* __REDQ_RPCPROXY_H_VERSION__ */\n");
  print_proxy( "\n");
  print_proxy( "#define __midl_proxy\n");
  print_proxy( "#include \"objbase.h\"\n");
  print_proxy( "#include \"rpcproxy.h\"\n");
  print_proxy( "#ifndef __RPCPROXY_H_VERSION__\n");
  print_proxy( "#error This code needs a newer version of rpcproxy.h\n");
  print_proxy( "#endif /* __RPCPROXY_H_VERSION__ */\n");
  print_proxy( "\n");
  print_proxy( "#include \"%s\"\n", header_name);
  print_proxy( "\n");
  write_formatstringsdecl(proxy, indent, stmts, need_proxy);
  write_stubdescproto();
}

static void clear_output_vars( const var_list_t *args )
{
  const var_t *arg;

  if (!args) return;
  LIST_FOR_EACH_ENTRY( arg, args, const var_t, entry )
  {
    if (is_attr(arg->attrs, ATTR_OUT) && !is_attr(arg->attrs, ATTR_IN)) {
      print_proxy( "if(%s)\n", arg->name );
      indent++;
      print_proxy( "MIDL_memset( %s, 0, sizeof( *%s ));\n", arg->name, arg->name );
      indent--;
    }
  }
}

int is_var_ptr(const var_t *v)
{
  return is_ptr(v->type);
}

int cant_be_null(const var_t *v)
{
  /* Search backwards for the most recent pointer attribute.  */
  const attr_list_t *attrs = v->attrs;
  const type_t *type = v->type;

  if (! attrs && type)
  {
    attrs = type->attrs;
    type = type->ref;
  }

  while (attrs)
  {
    int t = get_attrv(attrs, ATTR_POINTERTYPE);

    if (t == RPC_FC_FP || t == RPC_FC_OP || t == RPC_FC_UP)
      return 0;

    if (t == RPC_FC_RP)
      return 1;

    if (type)
    {
      attrs = type->attrs;
      type = type->ref;
    }
    else
      attrs = NULL;
  }

  return 1;                             /* Default is RPC_FC_RP.  */
}

static void proxy_check_pointers( const var_list_t *args )
{
  const var_t *arg;

  if (!args) return;
  LIST_FOR_EACH_ENTRY( arg, args, const var_t, entry )
  {
    if (is_var_ptr(arg) && cant_be_null(arg)) {
        print_proxy( "if(!%s)\n", arg->name );
        indent++;
        print_proxy( "RpcRaiseException(RPC_X_NULL_REF_POINTER);\n");
        indent--;
    }
  }
}

static void free_variable( const var_t *arg )
{
  unsigned int type_offset = arg->type->typestring_offset;
  expr_t *iid;
  type_t *type = arg->type;
  expr_t *size = get_size_is_expr(type, arg->name);

  if (size)
  {
    print_proxy( "_StubMsg.MaxCount = " );
    write_expr(proxy, size, 0, 1, NULL, NULL);
    fprintf(proxy, ";\n\n");
    print_proxy( "NdrClearOutParameters( &_StubMsg, ");
    fprintf(proxy, "&__MIDL_TypeFormatString.Format[%u], ", type_offset );
    fprintf(proxy, "(void*)%s );\n", arg->name );
    return;
  }

  switch( type->type )
  {
  case RPC_FC_BYTE:
  case RPC_FC_CHAR:
  case RPC_FC_WCHAR:
  case RPC_FC_SHORT:
  case RPC_FC_USHORT:
  case RPC_FC_ENUM16:
  case RPC_FC_LONG:
  case RPC_FC_ULONG:
  case RPC_FC_ENUM32:
  case RPC_FC_STRUCT:
    break;

  case RPC_FC_FP:
  case RPC_FC_IP:
    iid = get_attrp( arg->attrs, ATTR_IIDIS );
    if( iid )
    {
      print_proxy( "_StubMsg.MaxCount = (unsigned long) " );
      write_expr(proxy, iid, 1, 1, NULL, NULL);
      print_proxy( ";\n\n" );
    }
    print_proxy( "NdrClearOutParameters( &_StubMsg, ");
    fprintf(proxy, "&__MIDL_TypeFormatString.Format[%u], ", type_offset );
    fprintf(proxy, "(void*)%s );\n", arg->name );
    break;

  default:
    print_proxy("/* FIXME: %s code for %s type %d missing */\n", __FUNCTION__, arg->name, type->type );
  }
}

static void proxy_free_variables( var_list_t *args )
{
  const var_t *arg;

  if (!args) return;
  LIST_FOR_EACH_ENTRY( arg, args, const var_t, entry )
    if (is_attr(arg->attrs, ATTR_OUT))
    {
      free_variable( arg );
      fprintf(proxy, "\n");
    }
}

static void gen_proxy(type_t *iface, const func_t *cur, int idx,
                      unsigned int proc_offset)
{
  var_t *def = cur->def;
  int has_ret = !is_void(get_func_return_type(cur));
  int has_full_pointer = is_full_pointer_function(cur);
  const char *callconv = get_attrp(def->type->attrs, ATTR_CALLCONV);
  if (!callconv) callconv = "";

  indent = 0;
  write_type_decl_left(proxy, get_func_return_type(cur));
  print_proxy( " %s %s_", callconv, iface->name);
  write_name(proxy, def);
  print_proxy( "_Proxy(\n");
  write_args(proxy, cur->args, iface->name, 1, TRUE);
  print_proxy( ")\n");
  print_proxy( "{\n");
  indent ++;
  /* local variables */
  if (has_ret) {
    print_proxy( "" );
    write_type_decl_left(proxy, get_func_return_type(cur));
    print_proxy( " _RetVal;\n");
  }
  print_proxy( "RPC_MESSAGE _RpcMessage;\n" );
  print_proxy( "MIDL_STUB_MESSAGE _StubMsg;\n" );
  if (has_ret) {
    if (decl_indirect(get_func_return_type(cur)))
      print_proxy("void *_p_%s = &%s;\n",
                 "_RetVal", "_RetVal");
  }
  print_proxy( "\n");

  if (has_full_pointer)
    write_full_pointer_init(proxy, indent, cur, FALSE);

  /* FIXME: trace */
  clear_output_vars( cur->args );

  print_proxy( "RpcTryExcept\n" );
  print_proxy( "{\n" );
  indent++;
  print_proxy( "NdrProxyInitialize(This, &_RpcMessage, &_StubMsg, &Object_StubDesc, %d);\n", idx);
  proxy_check_pointers( cur->args );

  print_proxy( "RpcTryFinally\n" );
  print_proxy( "{\n" );
  indent++;

  write_remoting_arguments(proxy, indent, cur, PASS_IN, PHASE_BUFFERSIZE);

  print_proxy( "NdrProxyGetBuffer(This, &_StubMsg);\n" );

  write_remoting_arguments(proxy, indent, cur, PASS_IN, PHASE_MARSHAL);

  print_proxy( "NdrProxySendReceive(This, &_StubMsg);\n" );
  fprintf(proxy, "\n");
  print_proxy( "_StubMsg.BufferStart = _RpcMessage.Buffer;\n" );
  print_proxy( "_StubMsg.BufferEnd   = _StubMsg.BufferStart + _RpcMessage.BufferLength;\n\n" );

  print_proxy("if ((_RpcMessage.DataRepresentation & 0xffff) != NDR_LOCAL_DATA_REPRESENTATION)\n");
  indent++;
  print_proxy("NdrConvert( &_StubMsg, &__MIDL_ProcFormatString.Format[%u]);\n", proc_offset );
  indent--;
  fprintf(proxy, "\n");

  write_remoting_arguments(proxy, indent, cur, PASS_OUT, PHASE_UNMARSHAL);

  if (has_ret)
  {
      if (decl_indirect(get_func_return_type(cur)))
          print_proxy("MIDL_memset(&%s, 0, sizeof(%s));\n", "_RetVal", "_RetVal");
      else if (is_ptr(get_func_return_type(cur)) || is_array(get_func_return_type(cur)))
          print_proxy("%s = 0;\n", "_RetVal");
      write_remoting_arguments(proxy, indent, cur, PASS_RETURN, PHASE_UNMARSHAL);
  }

  indent--;
  print_proxy( "}\n");
  print_proxy( "RpcFinally\n" );
  print_proxy( "{\n" );
  indent++;
  if (has_full_pointer)
    write_full_pointer_free(proxy, indent, cur);
  print_proxy( "NdrProxyFreeBuffer(This, &_StubMsg);\n" );
  indent--;
  print_proxy( "}\n");
  print_proxy( "RpcEndFinally\n" );
  indent--;
  print_proxy( "}\n" );
  print_proxy( "RpcExcept(_StubMsg.dwStubPhase != PROXY_SENDRECEIVE)\n" );
  print_proxy( "{\n" );
  if (has_ret) {
    indent++;
    proxy_free_variables( cur->args );
    print_proxy( "_RetVal = NdrProxyErrorHandler(RpcExceptionCode());\n" );
    indent--;
  }
  print_proxy( "}\n" );
  print_proxy( "RpcEndExcept\n" );

  if (has_ret) {
    print_proxy( "return _RetVal;\n" );
  }
  indent--;
  print_proxy( "}\n");
  print_proxy( "\n");
}

static void gen_stub(type_t *iface, const func_t *cur, const char *cas,
                     unsigned int proc_offset)
{
  var_t *def = cur->def;
  const var_t *arg;
  int has_ret = !is_void(get_func_return_type(cur));
  int has_full_pointer = is_full_pointer_function(cur);

  indent = 0;
  print_proxy( "void __RPC_STUB %s_", iface->name);
  write_name(proxy, def);
  print_proxy( "_Stub(\n");
  indent++;
  print_proxy( "IRpcStubBuffer* This,\n");
  print_proxy( "IRpcChannelBuffer *_pRpcChannelBuffer,\n");
  print_proxy( "PRPC_MESSAGE _pRpcMessage,\n");
  print_proxy( "DWORD* _pdwStubPhase)\n");
  indent--;
  print_proxy( "{\n");
  indent++;
  print_proxy("%s * _This = (%s*)((CStdStubBuffer*)This)->pvServerObject;\n", iface->name, iface->name);
  print_proxy("MIDL_STUB_MESSAGE _StubMsg;\n");
  declare_stub_args( proxy, indent, cur );
  fprintf(proxy, "\n");

  /* FIXME: trace */

  print_proxy("NdrStubInitialize(_pRpcMessage, &_StubMsg, &Object_StubDesc, _pRpcChannelBuffer);\n");
  fprintf(proxy, "\n");

  write_parameters_init(proxy, indent, cur);

  print_proxy("RpcTryFinally\n");
  print_proxy("{\n");
  indent++;
  if (has_full_pointer)
    write_full_pointer_init(proxy, indent, cur, TRUE);
  print_proxy("if ((_pRpcMessage->DataRepresentation & 0xffff) != NDR_LOCAL_DATA_REPRESENTATION)\n");
  indent++;
  print_proxy("NdrConvert( &_StubMsg, &__MIDL_ProcFormatString.Format[%u]);\n", proc_offset );
  indent--;
  fprintf(proxy, "\n");

  write_remoting_arguments(proxy, indent, cur, PASS_IN, PHASE_UNMARSHAL);
  fprintf(proxy, "\n");

  assign_stub_out_args( proxy, indent, cur );

  print_proxy("*_pdwStubPhase = STUB_CALL_SERVER;\n");
  fprintf(proxy, "\n");
  print_proxy("");
  if (has_ret) fprintf(proxy, "_RetVal = ");
  if (cas) fprintf(proxy, "%s_%s_Stub", iface->name, cas);
  else
  {
      fprintf(proxy, "_This->lpVtbl->");
      write_name(proxy, def);
  }
  fprintf(proxy, "(_This");

  if (cur->args)
  {
      LIST_FOR_EACH_ENTRY( arg, cur->args, const var_t, entry )
      {
          fprintf(proxy, ", ");
          if (arg->type->declarray)
              fprintf(proxy, "*");
          write_name(proxy, arg);
      }
  }
  fprintf(proxy, ");\n");
  fprintf(proxy, "\n");
  print_proxy("*_pdwStubPhase = STUB_MARSHAL;\n");
  fprintf(proxy, "\n");

  write_remoting_arguments(proxy, indent, cur, PASS_OUT, PHASE_BUFFERSIZE);

  if (!is_void(get_func_return_type(cur)))
    write_remoting_arguments(proxy, indent, cur, PASS_RETURN, PHASE_BUFFERSIZE);

  print_proxy("NdrStubGetBuffer(This, _pRpcChannelBuffer, &_StubMsg);\n");

  write_remoting_arguments(proxy, indent, cur, PASS_OUT, PHASE_MARSHAL);
  fprintf(proxy, "\n");

  /* marshall the return value */
  if (!is_void(get_func_return_type(cur)))
    write_remoting_arguments(proxy, indent, cur, PASS_RETURN, PHASE_MARSHAL);

  indent--;
  print_proxy("}\n");
  print_proxy("RpcFinally\n");
  print_proxy("{\n");

  write_remoting_arguments(proxy, indent+1, cur, PASS_OUT, PHASE_FREE);

  if (has_full_pointer)
    write_full_pointer_free(proxy, indent, cur);

  print_proxy("}\n");
  print_proxy("RpcEndFinally\n");

  print_proxy("_pRpcMessage->BufferLength = _StubMsg.Buffer - (unsigned char *)_pRpcMessage->Buffer;\n");
  indent--;

  print_proxy("}\n");
  print_proxy("\n");
}

static int write_proxy_methods(type_t *iface)
{
  const func_t *cur;
  int i = 0;

  if (iface->ref) i = write_proxy_methods(iface->ref);
  if (iface->funcs) LIST_FOR_EACH_ENTRY( cur, iface->funcs, const func_t, entry ) {
    var_t *def = cur->def;
    if (!is_callas(def->attrs)) {
      if (i) fprintf(proxy, ",\n");
      print_proxy( "%s_", iface->name);
      write_name(proxy, def);
      fprintf(proxy, "_Proxy");
      i++;
    }
  }
  return i;
}

static int write_stub_methods(type_t *iface)
{
  const func_t *cur;
  int i = 0;

  if (iface->ref) i = write_stub_methods(iface->ref);
  else return i; /* skip IUnknown */

  if (iface->funcs) LIST_FOR_EACH_ENTRY( cur, iface->funcs, const func_t, entry ) {
    var_t *def = cur->def;
    if (!is_local(def->attrs)) {
      if (i) fprintf(proxy,",\n");
      print_proxy( "%s_", iface->name);
      write_name(proxy, def);
      fprintf(proxy, "_Stub");
      i++;
    }
  }
  return i;
}

static void write_proxy(type_t *iface, unsigned int *proc_offset)
{
  int midx = -1, stubs;
  const func_t *cur;

  if (!iface->funcs) return;

  /* FIXME: check for [oleautomation], shouldn't generate proxies/stubs if specified */

  fprintf(proxy, "/*****************************************************************************\n");
  fprintf(proxy, " * %s interface\n", iface->name);
  fprintf(proxy, " */\n");
  LIST_FOR_EACH_ENTRY( cur, iface->funcs, const func_t, entry )
  {
    const var_t *def = cur->def;
    if (!is_local(def->attrs)) {
      const var_t *cas = is_callas(def->attrs);
      const char *cname = cas ? cas->name : NULL;
      int idx = cur->idx;
      if (cname) {
          const func_t *m;
          LIST_FOR_EACH_ENTRY( m, iface->funcs, const func_t, entry )
              if (!strcmp(m->def->name, cname))
              {
                  idx = m->idx;
                  break;
              }
      }
      gen_proxy(iface, cur, idx, *proc_offset);
      gen_stub(iface, cur, cname, *proc_offset);
      *proc_offset += get_size_procformatstring_func( cur );
      if (midx == -1) midx = idx;
      else if (midx != idx) error("method index mismatch in write_proxy\n");
      midx++;
    }
  }

  /* proxy vtable */
  print_proxy( "static const CINTERFACE_PROXY_VTABLE(%d) _%sProxyVtbl =\n", midx, iface->name);
  print_proxy( "{\n");
  indent++;
  print_proxy( "{\n", iface->name);
  indent++;
  print_proxy( "&IID_%s,\n", iface->name);
  indent--;
  print_proxy( "},\n");
  print_proxy( "{\n");
  indent++;
  write_proxy_methods(iface);
  fprintf(proxy, "\n");
  indent--;
  print_proxy( "}\n");
  indent--;
  print_proxy( "};\n");
  fprintf(proxy, "\n\n");

  /* stub vtable */
  print_proxy( "static const PRPC_STUB_FUNCTION %s_table[] =\n", iface->name);
  print_proxy( "{\n");
  indent++;
  stubs = write_stub_methods(iface);
  fprintf(proxy, "\n");
  indent--;
  fprintf(proxy, "};\n");
  print_proxy( "\n");
  print_proxy( "static const CInterfaceStubVtbl _%sStubVtbl =\n", iface->name);
  print_proxy( "{\n");
  indent++;
  print_proxy( "{\n");
  indent++;
  print_proxy( "&IID_%s,\n", iface->name);
  print_proxy( "0,\n");
  print_proxy( "%d,\n", stubs+3);
  print_proxy( "&%s_table[-3],\n", iface->name);
  indent--;
  print_proxy( "},\n", iface->name);
  print_proxy( "{\n");
  indent++;
  print_proxy( "CStdStubBuffer_METHODS\n");
  indent--;
  print_proxy( "}\n");
  indent--;
  print_proxy( "};\n");
  print_proxy( "\n");
}

static int does_any_iface(const statement_list_t *stmts, type_pred_t pred)
{
  const statement_t *stmt;

  if (stmts)
    LIST_FOR_EACH_ENTRY(stmt, stmts, const statement_t, entry)
    {
      if (stmt->type == STMT_LIBRARY)
      {
          if (does_any_iface(stmt->u.lib->stmts, pred))
              return TRUE;
      }
      else if (stmt->type == STMT_TYPE && stmt->u.type->type == RPC_FC_IP)
      {
        if (pred(stmt->u.type))
          return TRUE;
      }
    }

  return FALSE;
}

int need_proxy(const type_t *iface)
{
  return is_object(iface->attrs) && !is_local(iface->attrs);
}

int need_stub(const type_t *iface)
{
  return !is_object(iface->attrs) && !is_local(iface->attrs);
}

int need_proxy_file(const statement_list_t *stmts)
{
  return does_any_iface(stmts, need_proxy);
}

int need_stub_files(const statement_list_t *stmts)
{
  return does_any_iface(stmts, need_stub);
}

static void write_proxy_stmts(const statement_list_t *stmts, unsigned int *proc_offset)
{
  const statement_t *stmt;
  if (stmts) LIST_FOR_EACH_ENTRY( stmt, stmts, const statement_t, entry )
  {
    if (stmt->type == STMT_LIBRARY)
      write_proxy_stmts(stmt->u.lib->stmts, proc_offset);
    else if (stmt->type == STMT_TYPE && stmt->u.type->type == RPC_FC_IP)
    {
      if (need_proxy(stmt->u.type))
        write_proxy(stmt->u.type, proc_offset);
    }
  }
}

static void write_proxy_iface_name_format(const statement_list_t *stmts, const char *format)
{
  const statement_t *stmt;
  if (stmts) LIST_FOR_EACH_ENTRY( stmt, stmts, const statement_t, entry )
  {
    if (stmt->type == STMT_LIBRARY)
      write_proxy_iface_name_format(stmt->u.lib->stmts, format);
    else if (stmt->type == STMT_TYPE && stmt->u.type->type == RPC_FC_IP)
    {
      type_t *iface = stmt->u.type;
      if (iface->ref && iface->funcs && need_proxy(iface))
        fprintf(proxy, format, iface->name);
    }
  }
}

static void write_iid_lookup(const statement_list_t *stmts, const char *file_id, int *c)
{
  const statement_t *stmt;
  if (stmts) LIST_FOR_EACH_ENTRY( stmt, stmts, const statement_t, entry )
  {
    if (stmt->type == STMT_LIBRARY)
      write_iid_lookup(stmt->u.lib->stmts, file_id, c);
    else if (stmt->type == STMT_TYPE && stmt->u.type->type == RPC_FC_IP)
    {
      type_t *iface = stmt->u.type;
      if(iface->ref && iface->funcs && need_proxy(iface))
      {
        fprintf(proxy, "    if (!_%s_CHECK_IID(%d))\n", file_id, *c);
        fprintf(proxy, "    {\n");
        fprintf(proxy, "        *pIndex = %d;\n", *c);
        fprintf(proxy, "        return 1;\n");
        fprintf(proxy, "    }\n");
        (*c)++;
      }
    }
  }
}

void write_proxies(const statement_list_t *stmts)
{
  int expr_eval_routines;
  char *file_id = proxy_token;
  int c;
  unsigned int proc_offset = 0;

  if (!do_proxies) return;
  if (do_everything && !need_proxy_file(stmts)) return;

  init_proxy(stmts);
  if(!proxy) return;

  write_proxy_stmts(stmts, &proc_offset);

  expr_eval_routines = write_expr_eval_routines(proxy, proxy_token);
  if (expr_eval_routines)
      write_expr_eval_routine_list(proxy, proxy_token);
  write_user_quad_list(proxy);
  write_stubdesc(expr_eval_routines);

  print_proxy( "#if !defined(__RPC_WIN32__)\n");
  print_proxy( "#error Currently only Wine and WIN32 are supported.\n");
  print_proxy( "#endif\n");
  print_proxy( "\n");
  write_procformatstring(proxy, stmts, need_proxy);
  write_typeformatstring(proxy, stmts, need_proxy);

  fprintf(proxy, "static const CInterfaceProxyVtbl* const _%s_ProxyVtblList[] =\n", file_id);
  fprintf(proxy, "{\n");
  write_proxy_iface_name_format(stmts, "    (const CInterfaceProxyVtbl*)&_%sProxyVtbl,\n");
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "static const CInterfaceStubVtbl* const _%s_StubVtblList[] =\n", file_id);
  fprintf(proxy, "{\n");
  write_proxy_iface_name_format(stmts, "    (const CInterfaceStubVtbl*)&_%sStubVtbl,\n");
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "static PCInterfaceName const _%s_InterfaceNamesList[] =\n", file_id);
  fprintf(proxy, "{\n");
  write_proxy_iface_name_format(stmts, "    \"%s\",\n");
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "#define _%s_CHECK_IID(n) IID_GENERIC_CHECK_IID(_%s, pIID, n)\n", file_id, file_id);
  fprintf(proxy, "\n");
  fprintf(proxy, "int __stdcall _%s_IID_Lookup(const IID* pIID, int* pIndex)\n", file_id);
  fprintf(proxy, "{\n");
  c = 0;
  write_iid_lookup(stmts, file_id, &c);
  fprintf(proxy, "    return 0;\n");
  fprintf(proxy, "}\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "const ExtendedProxyFileInfo %s_ProxyFileInfo =\n", file_id);
  fprintf(proxy, "{\n");
  fprintf(proxy, "    (const PCInterfaceProxyVtblList*)&_%s_ProxyVtblList,\n", file_id);
  fprintf(proxy, "    (const PCInterfaceStubVtblList*)&_%s_StubVtblList,\n", file_id);
  fprintf(proxy, "    _%s_InterfaceNamesList,\n", file_id);
  fprintf(proxy, "    0,\n");
  fprintf(proxy, "    &_%s_IID_Lookup,\n", file_id);
  fprintf(proxy, "    %d,\n", c);
  fprintf(proxy, "    1,\n");
  fprintf(proxy, "    0,\n");
  fprintf(proxy, "    0,\n");
  fprintf(proxy, "    0,\n");
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");

  fclose(proxy);
}
