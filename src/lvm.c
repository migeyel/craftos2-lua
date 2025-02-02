/*
** $Id: lvm.c,v 2.155.1.1 2013/04/12 18:48:47 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lvm_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"



/* limit for table tag-method chains (to avoid loops) */
#define MAXTAGLOOP	100


#define resolverope(L, o) {if (ttisrope(o)) setsvalue(L, o, luaS_build(L, rawtrvalue(o)));}
#define resolvesubstr(L, o) {if (ttissubstr(o)) setsvalue(L, o, luaS_newlstr(L, getstr(ssvalue(o)->str) + ssvalue(o)->offset, ssvalue(o)->len));}


const TValue *luaV_tonumber (lua_State *L, const TValue *obj, TValue *n) {
  lua_Number num;
  if (ttisnumber(obj)) return obj;
  resolverope(L, obj);
  resolvesubstr(L, obj);
  if (ttisstring(obj) && luaO_str2d(svalue(obj), tsvalue(obj)->len, &num)) {
    setnvalue(n, num);
    return n;
  }
  else
    return NULL;
}


int luaV_tostring (lua_State *L, StkId obj) {
  if (ttisrope(obj)) {
    resolverope(L, obj);
    return 1;
  } else if (ttissubstr(obj)) {
    resolvesubstr(L, obj);
    return 1;
  } else if (ttisnumber(obj)) {
    char s[LUAI_MAXNUMBER2STR];
    lua_Number n = nvalue(obj);
    int l = lua_number2str(s, n);
    setsvalue2s(L, obj, luaS_newlstr(L, s, l));
    return 1;
  } else return 0;
}


static void traceexec (lua_State *L) {
  CallInfo *ci = L->ci;
  lu_byte mask = L->hookmask;
  int counthook = ((mask & LUA_MASKCOUNT) && L->hookcount == 0);
  if (counthook)
    resethookcount(L);  /* reset count */
  if (ci->hook != 0xFF) {  /* called hook last time? */
    if (ci->hook == LUA_HOOKLINE) {
      ci->hook = 0xFF;
      return;  /* do not call hook again (VM yielded, so it did not move) */
    }
    ci->hook = 0xFF;
  } else if (counthook)
    luaD_hook(L, LUA_HOOKCOUNT, -1);  /* call count hook */
  if (mask & LUA_MASKLINE) {
    Proto *p = ci_func(ci)->p;
    int npc = pcRel(ci->u.l.savedpc, p);
    int newline = getfuncline(p, npc);
    if (npc == 0 ||  /* call linehook when enter a new function, */
        ci->u.l.savedpc <= L->oldpc ||  /* when jump back (loop), or when */
        newline != getfuncline(p, pcRel(L->oldpc, p)))  /* enter a new line */
      luaD_hook(L, LUA_HOOKLINE, newline);  /* call line hook */
  }
  L->oldpc = ci->u.l.savedpc;
  if (L->status == LUA_YIELD) {  /* did hook yield? */
    if (counthook)
      L->hookcount = 1;  /* undo decrement to zero */
    ci->u.l.savedpc--;  /* undo increment (resume will increment it again) */
    ci->func = L->top - 1;  /* protect stack below results */
    luaD_throw(L, LUA_YIELD);
  }
}


static void callTM (lua_State *L, const TValue *f, const TValue *p1,
                    const TValue *p2, TValue *p3, int hasres) {
  ptrdiff_t result = savestack(L, p3);
  setobj2s(L, L->top++, f);  /* push function */
  setobj2s(L, L->top++, p1);  /* 1st argument */
  setobj2s(L, L->top++, p2);  /* 2nd argument */
  if (!hasres)  /* no result? 'p3' is third argument */
    setobj2s(L, L->top++, p3);  /* 3rd argument */
  /* metamethod may yield only when called from Lua code */
  luaD_call(L, L->top - (4 - hasres), hasres, isLua(L->ci));
  if (hasres) {  /* if has result, move it to its place */
    p3 = restorestack(L, result);
    setobjs2s(L, p3, --L->top);
  }
}


void luaV_gettable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  int loop;
  resolverope(L, key);
  resolvesubstr(L, key);
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      const TValue *res = luaH_get(L, h, key); /* do a primitive get */
      if (!ttisnil(res) ||  /* result is not nil? */
          (tm = fasttm(L, h->metatable, TM_INDEX)) == NULL) { /* or no TM? */
        setobj2s(L, val, res);
        return;
      }
      /* else will try the tag method */
    }
    else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_INDEX)))
      luaG_typeerror(L, t, "index");
    if (ttisfunction(tm)) {
      callTM(L, tm, t, key, val, 1);
      return;
    }
    t = tm;  /* else repeat with 'tm' */
  }
  luaG_runerror(L, "loop in gettable");
}


void luaV_settable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  int loop;
  TValue temp;
  resolverope(L, key);
  resolvesubstr(L, key);
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      TValue *oldval = cast(TValue *, luaH_get(L, h, key));
      /* if previous value is not nil, there must be a previous entry
         in the table; moreover, a metamethod has no relevance */
      if (!ttisnil(oldval) ||
         /* previous value is nil; must check the metamethod */
         ((tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL &&
         /* no metamethod; is there a previous entry in the table? */
         (oldval != luaO_nilobject ||
         /* no previous entry; must create one. (The next test is
            always true; we only need the assignment.) */
         (oldval = luaH_newkey(L, h, key), 1)))) {
        /* no metamethod and (now) there is an entry with given key */
        setobj2t(L, oldval, val);  /* assign new value to that entry */
        invalidateTMcache(h);
        luaC_barrierback(L, obj2gco(h), val);
        return;
      }
      /* else will try the metamethod */
    }
    else  /* not a table; check metamethod */
      if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
        luaG_typeerror(L, t, "index");
    /* there is a metamethod */
    if (ttisfunction(tm)) {
      callTM(L, tm, t, key, val, 0);
      return;
    }
    t = tm;  /* else repeat with 'tm' */
  }
  luaG_runerror(L, "loop in settable");
}


static int call_binTM (lua_State *L, const TValue *p1, const TValue *p2,
                       StkId res, TMS event) {
  const TValue *tm = luaT_gettmbyobj(L, p1, event);  /* try first operand */
  if (ttisnil(tm))
    tm = luaT_gettmbyobj(L, p2, event);  /* try second operand */
  if (ttisnil(tm)) return 0;
  callTM(L, tm, p1, p2, res, 1);
  return 1;
}


static const TValue *get_equalTM (lua_State *L, Table *mt1, Table *mt2,
                                  TMS event) {
  const TValue *tm1 = fasttm(L, mt1, event);
  const TValue *tm2;
  if (tm1 == NULL) return NULL;  /* no metamethod */
  if (mt1 == mt2) return tm1;  /* same metatables => same metamethods */
  tm2 = fasttm(L, mt2, event);
  if (tm2 == NULL) return NULL;  /* no metamethod */
  if (luaV_rawequalobj(tm1, tm2))  /* same metamethods? */
    return tm1;
  return NULL;
}


static int call_orderTM (lua_State *L, const TValue *p1, const TValue *p2,
                         TMS event) {
  if (!call_binTM(L, p1, p2, L->top, event))
    return -1;  /* no metamethod */
  else
    return !l_isfalse(L->top);
}


static int l_strcmp (const TString *ls, const TString *rs) {
  const char *l = getstr(ls);
  size_t ll = ls->tsv.len;
  const char *r = getstr(rs);
  size_t lr = rs->tsv.len;
  for (;;) {
    int temp = strcmp(l, r);
    if (temp != 0) return temp;
    else {  /* strings are equal up to a `\0' */
      size_t len = strlen(l);  /* index of first `\0' in both strings */
      if (len == lr)  /* r is finished? */
        return (len == ll) ? 0 : 1;
      else if (len == ll)  /* l is finished? */
        return -1;  /* l is smaller than r (because r is not finished) */
      /* both strings longer than `len'; go on comparing (after the `\0') */
      len++;
      l += len; ll -= len; r += len; lr -= len;
    }
  }
}


static int l_substrcmp (const TString *ls, const TString *rs) {
  const char *l = getstr(ls->tss.str) + ls->tss.offset;
  size_t ll = ls->tss.len;
  const char *r = getstr(rs->tss.str) + rs->tss.offset;
  size_t lr = rs->tss.len;
  for (;;) {
    int temp = strncmp(l, r, lr > ll ? ll : lr);
    if (temp != 0) return temp;
    else {  /* strings are equal up to a `\0' */
      size_t len = strlen(l);  /* index of first `\0' in both strings */
      if (len == lr)  /* r is finished? */
        return (len == ll) ? 0 : 1;
      else if (len == ll)  /* l is finished? */
        return -1;  /* l is smaller than r (because r is not finished) */
      else if (len > ll && l > lr)  /* the next `\0' is beyond the end of the substring */
        return 0;  /* l or r is finished */
      /* both strings longer than `len'; go on comparing (after the `\0') */
      len++;
      l += len; ll -= len; r += len; lr -= len;
    }
  }
}


int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  resolverope(L, l);
  resolverope(L, r);
  if (ttissubstr(l) != ttissubstr(r)) {
    resolvesubstr(L, l);
    resolvesubstr(L, r);
  }
  if (ttype(l) != ttype(r))
    luaG_ordererror(L, l, r);
  else if (ttisnumber(l))
    return luai_numlt(L, nvalue(l), nvalue(r));
  else if (ttisstring(l))
    return l_strcmp(rawtsvalue(l), rawtsvalue(r)) < 0;
  else if (ttissubstr(l))
    return l_substrcmp(rawssvalue(l), rawssvalue(r)) < 0;
  else if ((res = call_orderTM(L, l, r, TM_LT)) != -1)
    return res;
  luaG_ordererror(L, l, r);
}


int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  resolverope(L, l);
  resolverope(L, r);
  if (ttissubstr(l) != ttissubstr(r)) {
    resolvesubstr(L, l);
    resolvesubstr(L, r);
  }
  if (ttype(l) != ttype(r))
    luaG_ordererror(L, l, r);
  else if (ttisnumber(l))
    return luai_numle(L, nvalue(l), nvalue(r));
  else if (ttisstring(l))
    return l_strcmp(rawtsvalue(l), rawtsvalue(r)) <= 0;
  else if (ttissubstr(l))
    return l_substrcmp(rawssvalue(l), rawssvalue(r)) <= 0;
  else if ((res = call_orderTM(L, l, r, TM_LE)) != -1)  /* first try `le' */
    return res;
  else if ((res = call_orderTM(L, r, l, TM_LT)) < 0)  /* else try `lt' */
    luaG_ordererror(L, l, r);
  return !res;
}


/*
** equality of Lua values. L == NULL means raw equality (no metamethods)
*/
int luaV_equalobj_ (lua_State *L, const TValue *t1, const TValue *t2) {
  const TValue *tm;
  resolverope(L, t1);
  resolvesubstr(L, t1);
  resolverope(L, t2);
  resolvesubstr(L, t2);
  lua_assert(ttype(t1) == ttype(t2));
  switch (ttype(t1)) {
    case LUA_TNIL: return 1;
    case LUA_TNUMBER: return luai_numeq(nvalue(t1), nvalue(t2));
    case LUA_TBOOLEAN: return bvalue(t1) == bvalue(t2);  /* true must be 1 !! */
    case LUA_TLIGHTUSERDATA: return pvalue(t1) == pvalue(t2);
    case LUA_TLCF: return fvalue(t1) == fvalue(t2);
    case LUA_TSHRSTR: return eqshrstr(rawtsvalue(t1), rawtsvalue(t2));
    case LUA_TLNGSTR: return luaS_eqlngstr(rawtsvalue(t1), rawtsvalue(t2));
    case LUA_TUSERDATA: {
      if (uvalue(t1) == uvalue(t2)) return 1;
      else if (L == NULL) return 0;
      tm = get_equalTM(L, uvalue(t1)->metatable, uvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    case LUA_TTABLE: {
      if (hvalue(t1) == hvalue(t2)) return 1;
      else if (L == NULL) return 0;
      tm = get_equalTM(L, hvalue(t1)->metatable, hvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    default:
      lua_assert(iscollectable(t1));
      return gcvalue(t1) == gcvalue(t2);
  }
  if (tm == NULL) return 0;  /* no TM? */
  callTM(L, tm, t1, t2, L->top, 1);  /* call TM */
  return !l_isfalse(L->top);
}


static TString *makerope(lua_State *L, StkId start, int len) {
  switch (len) {
    case 1: return rawtsvalue(start); /* this should never happen */
    case 2: return luaS_concat(L, rawtsvalue(start), rawtsvalue(start + 1));
    case 3: return luaS_concat(L, rawtsvalue(start), luaS_concat(L, rawtsvalue(start + 1), rawtsvalue(start + 2)));
    default: return luaS_concat(L, makerope(L, start, len / 2), makerope(L, start + (len / 2), len - (len / 2)));
  }
}


void luaV_concat (lua_State *L, int total) {
  do {
    StkId top = L->top;
    int n = 2;  /* number of elements handled in this pass (at least 2) */
    if (!(ttisstring(top-2) || ttisnumber(top-2)) || !tostring(L, top-1)) {
      if (!call_binTM(L, top-2, top-1, top-2, TM_CONCAT))
        luaG_concaterror(L, top-2, top-1);
    }
    else if ((ttisrope(top-1) ? trvalue(top-1)->len : (ttissubstr(top-1) ? ssvalue(top-1)->len : tsvalue(top-1)->len)) == 0)  /* second operand is empty? */
      (void)tostring(L, top - 2);  /* result is first operand */
    else if (ttisstring(top-2) && (ttisrope(top-2) ? trvalue(top-2)->len : (ttissubstr(top-2) ? ssvalue(top-2)->len : tsvalue(top-2)->len)) == 0) {
      setobjs2s(L, top - 2, top - 1);  /* result is second op. */
    }
    else {
      /* at least two non-empty string values; get as many as possible */
      size_t tl = tsvalue(top-1)->len;
      /* collect total length */
      for (n = 1; n < total && (ttisrope(top-n-1) || ttissubstr(top-n-1) || tostring(L, top-n-1)); n++) {
        size_t l = ttisrope(top-n-1) ? trvalue(top-n-1)->len : (ttissubstr(top-n-1) ? ssvalue(top-n-1)->len : tsvalue(top-n-1)->len);
        if (l >= MAX_SIZET - tl) luaG_runerror(L, "string length overflow");
        tl += l;
      }
      setrvalue(L, top-n, makerope(L, top-n, n));
    }
    total -= n-1;  /* got 'n' strings to create 1 new */
    L->top -= n-1;  /* popped 'n' strings and pushed one */
  } while (total > 1);  /* repeat until only 1 result left */
}


void luaV_objlen (lua_State *L, StkId ra, const TValue *rb) {
  const TValue *tm;
  switch (ttype(rb)) {
    case LUA_TTABLE: {
      Table *h = hvalue(rb);
      tm = fasttm(L, h->metatable, TM_LEN);
      if (tm) break;  /* metamethod? break switch to call it */
      setnvalue(ra, cast_num(luaH_getn(h)));  /* else primitive len */
      return;
    }
    case LUA_TLNGSTR: case LUA_TSHRSTR: {
      setnvalue(ra, cast_num(tsvalue(rb)->len));
      return;
    }
    case LUA_TROPSTR: {
      setnvalue(ra, cast_num(trvalue(rb)->len));
      return;
    }
    case LUA_TSUBSTR: {
      setnvalue(ra, cast_num(ssvalue(rb)->len));
      return;
    }
    default: {  /* try metamethod */
      tm = luaT_gettmbyobj(L, rb, TM_LEN);
      if (ttisnil(tm))  /* no metamethod? */
        luaG_typeerror(L, rb, "get length of");
      break;
    }
  }
  callTM(L, tm, rb, rb, ra, 1);
}

static lua_Number luai_nummod(lua_State *L, lua_Number a, lua_Number b) {
  lua_Number q = fmod(a, b);
  if (q == 0) return 0;
  else if ((a < 0) != (b < 0)) return q + b;
  return q;
}


void luaV_arith (lua_State *L, StkId ra, const TValue *rb,
                 const TValue *rc, TMS op) {
  TValue tempb, tempc;
  const TValue *b, *c;
  if ((b = luaV_tonumber(L, rb, &tempb)) != NULL &&
      (c = luaV_tonumber(L, rc, &tempc)) != NULL) {
    lua_Number res = luaO_arith(op - TM_ADD + LUA_OPADD, nvalue(b), nvalue(c));
    setnvalue(ra, res);
  }
  else if (!call_binTM(L, rb, rc, ra, op))
    luaG_aritherror(L, rb, rc);
}


/*
** check whether cached closure in prototype 'p' may be reused, that is,
** whether there is a cached closure with the same upvalues needed by
** new closure to be created.
*/
static Closure *getcached (Proto *p, UpVal **encup, StkId base) {
  Closure *c = p->cache;
  if (c != NULL) {  /* is there a cached closure? */
    int nup = p->sizeupvalues;
    Upvaldesc *uv = p->upvalues;
    int i;
    for (i = 0; i < nup; i++) {  /* check whether it has right upvalues */
      TValue *v = uv[i].instack ? base + uv[i].idx : encup[uv[i].idx]->v;
      if (c->l.upvals[i]->v != v)
        return NULL;  /* wrong upvalue; cannot reuse closure */
    }
  }
  return c;  /* return cached closure (or NULL if no cached closure) */
}


/*
** create a new Lua closure, push it in the stack, and initialize
** its upvalues. Note that the call to 'luaC_barrierproto' must come
** before the assignment to 'p->cache', as the function needs the
** original value of that field.
*/
static void pushclosure (lua_State *L, Proto *p, UpVal **encup, StkId base,
                         StkId ra) {
  int nup = p->sizeupvalues;
  Upvaldesc *uv = p->upvalues;
  int i;
  Closure *ncl = luaF_newLclosure(L, nup);
  ncl->l.p = p;
  setclLvalue(L, ra, ncl);  /* anchor new closure in stack */
  for (i = 0; i < nup; i++) {  /* fill in its upvalues */
    if (uv[i].instack)  /* upvalue refers to local variable? */
      ncl->l.upvals[i] = luaF_findupval(L, base + uv[i].idx);
    else  /* get upvalue from enclosing function */
      ncl->l.upvals[i] = encup[uv[i].idx];
  }
  luaC_barrierproto(L, p, ncl);
  p->cache = ncl;  /* save it on cache for reuse */
}


/*
** finish execution of an opcode interrupted by an yield
*/
int luaV_finishOp (lua_State *L) {
  CallInfo *ci = L->ci;
  StkId base;
  Instruction inst;  /* interrupted instruction */
  OpCode op;
  if (ci->callstatus & CIST_HOOKED) {  /* hook yield w/continuation? */
    /* finish hook */
    L->allowhook = 1;
    ci->top = restorestack(L, ci->old_ci_top);
    L->top = restorestack(L, ci->old_top);
    ci->callstatus &= ~CIST_HOOKED;
    /* finish action after hook */
    switch (ci->hook) {
      case LUA_HOOKCOUNT:
      case LUA_HOOKLINE:
        ci->u.l.savedpc--;  /* reexecute instruction */
        return 0;  /* keep ci->hook so traceexec can avoid running the hook again */
      case LUA_HOOKCALL:
        ci->u.l.savedpc--;  /* correct 'pc' */
        break;
      case LUA_HOOKTAILCALL: {
        /* tail call: put called frame (n) in place of caller one (o) */
        CallInfo *nci = L->ci;  /* called frame */
        CallInfo *oci = nci->previous;  /* caller frame */
        StkId nfunc = nci->func;  /* called function */
        StkId ofunc = oci->func;  /* caller function */
        /* last stack slot filled by 'precall' */
        StkId lim = nci->u.l.base + getproto(nfunc)->numparams;
        int aux;
        ci->u.l.savedpc--;
        /* close all upvalues from previous call */
        if (clLvalue(ofunc)->p->sizep > 0) luaF_close(L, oci->u.l.base);
        /* move new frame into old one */
        for (aux = 0; nfunc + aux < lim; aux++)
          setobjs2s(L, ofunc + aux, nfunc + aux);
        oci->u.l.base = ofunc + (nci->u.l.base - nfunc);  /* correct base */
        oci->top = L->top = ofunc + (L->top - nfunc);  /* correct top */
        oci->u.l.savedpc = nci->u.l.savedpc;
        oci->callstatus |= CIST_TAIL;  /* function was tail called */
        ci = L->ci = oci;  /* remove new frame */
        lua_assert(L->top == oci->u.l.base + getproto(ofunc)->maxstacksize);
        break;
      } case LUA_HOOKRET:
        /* retry return with hooks disabled */
        L->allowhook = 0;
        luaD_poscall(L, restorestack(L, ci->old_fr));
        L->allowhook = 1;
        ci->hook = 0xFF;
        return 1;  /* don't execute after */
    }
    ci->hook = 0xFF;
    return 0;
  }
  base = ci->u.l.base;
  inst = *(ci->u.l.savedpc - 1);  /* interrupted instruction */
  op = GET_OPCODE(inst);
  switch (op) {  /* finish its execution */
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
    case OP_MOD: case OP_POW: case OP_UNM: case OP_LEN:
    case OP_GETTABUP: case OP_GETTABLE: case OP_SELF: {
      setobjs2s(L, base + GETARG_A(inst), --L->top);
      break;
    }
    case OP_LE: case OP_LT: case OP_EQ: {
      int res = !l_isfalse(L->top - 1);
      L->top--;
      /* metamethod should not be called when operand is K */
      lua_assert(!ISK(GETARG_B(inst)));
      if (op == OP_LE &&  /* "<=" using "<" instead? */
          ttisnil(luaT_gettmbyobj(L, base + GETARG_B(inst), TM_LE)))
        res = !res;  /* invert result */
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_JMP);
      if (res != GETARG_A(inst))  /* condition failed? */
        ci->u.l.savedpc++;  /* skip jump instruction */
      break;
    }
    case OP_CONCAT: {
      StkId top = L->top - 1;  /* top when 'call_binTM' was called */
      int b = GETARG_B(inst);      /* first element to concatenate */
      int total = cast_int(top - 1 - (base + b));  /* yet to concatenate */
      setobj2s(L, top - 2, top);  /* put TM result in proper position */
      if (total > 1) {  /* are there elements to concat? */
        L->top = top - 1;  /* top is one after last element (at top-2) */
        luaV_concat(L, total);  /* concat them (may yield again) */
      }
      /* move final result to final position */
      setobj2s(L, ci->u.l.base + GETARG_A(inst), L->top - 1);
      L->top = ci->top;  /* restore top */
      break;
    }
    case OP_TFORCALL: {
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_TFORLOOP);
      L->top = ci->top;  /* correct top */
      break;
    }
    case OP_CALL: {
      if (GETARG_C(inst) - 1 >= 0)  /* nresults >= 0? */
        L->top = ci->top;  /* adjust results */
      break;
    }
    case OP_TAILCALL: case OP_SETTABUP: case OP_SETTABLE:
      break;
    default: lua_assert(0);
  }
  return 0;
}


#ifdef LUA_DEBUG_VM
static void PrintString(const TString* ts)
{
 const char* s=getstr(ts);
 size_t i,n=ts->tsv.len;
 putchar('"');
 for (i=0; i<n; i++)
 {
  int c=s[i];
  switch (c)
  {
   case '"': printf("\\\""); break;
   case '\\': printf("\\\\"); break;
   case '\a': printf("\\a"); break;
   case '\b': printf("\\b"); break;
   case '\f': printf("\\f"); break;
   case '\n': printf("\\n"); break;
   case '\r': printf("\\r"); break;
   case '\t': printf("\\t"); break;
   case '\v': printf("\\v"); break;
   default:	if (isprint((unsigned char)c))
   			putchar(c);
		else
			printf("\\%03u",(unsigned char)c);
  }
 }
 putchar('"');
}

static void PrintConstant(const Proto* f, int i)
{
 const TValue* o=&f->k[i];
 switch (ttype(o))
 {
  case LUA_TNIL:
	printf("nil");
	break;
  case LUA_TBOOLEAN:
	printf(bvalue(o) ? "true" : "false");
	break;
  case LUA_TNUMBER:
	printf(LUA_NUMBER_FMT,nvalue(o));
	break;
  case LUA_TSTRING:
	PrintString(rawtsvalue(o));
	break;
  default:				/* cannot happen */
	printf("? type=%d",ttype(o));
	break;
 }
}

#define VOID(p)		((const void*)(p))
#define UPVALNAME(x) ((cl->p->upvalues[x].name) ? getstr(cl->p->upvalues[x].name) : "-")
#define MYK(x)		(-1-(x))
#endif



/*
** some macros for common tasks in `luaV_execute'
*/

#if !defined luai_runtimecheck
#define luai_runtimecheck(L, c)		/* void */
#endif


#define RA(i)	(base+GETARG_A(i))
/* to be used after possible stack reallocation */
#define RB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgR, base+GETARG_B(i))
#define RC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgR, base+GETARG_C(i))
#define RKB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_B(i)) ? k+INDEXK(GETARG_B(i)) : base+GETARG_B(i))
#define RKC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_C(i)) ? k+INDEXK(GETARG_C(i)) : base+GETARG_C(i))
#define KBx(i)  \
  (k + (GETARG_Bx(i) != 0 ? GETARG_Bx(i) - 1 : GETARG_Ax(*ci->u.l.savedpc++)))


/* execute a jump instruction */
#define dojump(ci,i,e) \
  { int a = GETARG_A(i); \
    if (a > 0) luaF_close(L, ci->u.l.base + a - 1); \
    ci->u.l.savedpc += GETARG_sBx(i) + e; }

/* for test instructions, execute the jump instruction that follows it */
#define donextjump(ci)	{ i = *ci->u.l.savedpc; dojump(ci, i, 1); }


#define Protect(x)	{ {x;}; base = ci->u.l.base; }

#define checkGC(L,c)  \
  Protect( luaC_condGC(L,{L->top = (c);  /* limit of live values */ \
                          luaC_step(L); \
                          L->top = ci->top;})  /* restore top */ \
           luai_threadyield(L); )


#define arith_op(op,tm) { \
        TValue *rb = RKB(i); \
        TValue *rc = RKC(i); \
        if (ttisnumber(rb) && ttisnumber(rc)) { \
          lua_Number nb = nvalue(rb), nc = nvalue(rc); \
          setnvalue(ra, op(L, nb, nc)); \
        } \
        else { Protect(luaV_arith(L, ra, rb, rc, tm)); } }


#define vmdispatch(o)	switch(o)
#define vmcase(l,b)	case l: {b}  break;
#define vmcasenb(l,b)	case l: {b}		/* nb = no break */

void luaV_execute (lua_State *L) {
  CallInfo *ci = L->ci;
  LClosure *cl;
  TValue *k;
  StkId base;
 newframe:  /* reentry point when frame changes (call/return) */
  lua_assert(ci == L->ci);
  cl = clLvalue(ci->func);
  k = cl->p->k;
  base = ci->u.l.base;
  /* main loop of interpreter */
  for (;;) {
    Instruction i = *(ci->u.l.savedpc++);
    StkId ra;
    if (G(L)->haltstate) {  /* exit if the state was halted */
      if (G(L)->haltstate == 2) {  /* throw an error instead of halting fully */
        int concat = 0;
        luaC_checkGC(L);
        if (G(L)->haltmessage) {
          if (isLua(ci) && ci_func(ci)->p->lineinfo) {
            char wheretemp[LUA_IDSIZE+20];  /* 20 extra characters should be enough for line number + 4 characters (:: <\0>) */
            size_t wheresize;
            luaO_chunkid(wheretemp, getstr(clvalue(ci->func)->l.p->source), LUA_IDSIZE);
            wheresize = strlen(wheretemp);
            wheresize += sprintf(wheretemp + wheresize, ":%d: ", getfuncline(ci_func(ci)->p, pcRel(ci->u.l.savedpc, ci_func(ci)->p)));
            setsvalue2s(L, L->top, luaS_newlstr(L, wheretemp, wheresize));
            L->top++;
            concat = 1;
          }
          setsvalue2s(L, L->top, luaS_newlstr(L, G(L)->haltmessage, strlen(G(L)->haltmessage)));
        }
        else {setnilvalue(L->top);}
        L->top++;
        if (concat) {
          luaV_concat(L, 2);
        }
        G(L)->haltstate = 0;
        G(L)->haltmessage = NULL;
        luaD_throw(L, LUA_ERRRUN);
      }
      return;
    }
    if ((L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) &&
        (--L->hookcount == 0 || L->hookmask & LUA_MASKLINE)) {
      Protect(traceexec(L));
    }
    /* WARNING: several calls may realloc the stack and invalidate `ra' */
    ra = RA(i);
    lua_assert(base == ci->u.l.base);
    lua_assert(base <= L->top && L->top < L->stack + L->stacksize);
#ifdef LUA_DEBUG_VM
    {
      OpCode o=GET_OPCODE(i);
      int a=GETARG_A(i);
      int b=GETARG_B(i);
      int c=GETARG_C(i);
      int ax = GETARG_Ax(i);
      int bx=GETARG_Bx(i);
      int sbx=GETARG_sBx(i);
      Instruction *pc=ci->u.l.savedpc-1;
      int line;
      if (pc - cl->p->code >= cl->p->sizecode) {
        printf("\t%d\t[-]\tOVERFLOW (!)\n", pc - cl->p->code + 1);
        goto debugend;
      }
      line=getfuncline(cl->p,pc - cl->p->code);
      printf("\t%d\t",pc-cl->p->code+1);
      if (line>0) printf("[%d]\t",line); else printf("[-]\t");
      printf("%-9s\t",luaP_opnames[o]);
      switch (getOpMode(o))
      {
      case iABC:
        printf("%d",a);
        if (getBMode(o)!=OpArgN) printf(" %d",ISK(b) ? (MYK(INDEXK(b))) : b);
        if (getCMode(o)!=OpArgN) printf(" %d",ISK(c) ? (MYK(INDEXK(c))) : c);
        break;
      case iABx:
        printf("%d",a);
        if (getBMode(o)==OpArgK) printf(" %d",MYK(bx));
        if (getBMode(o)==OpArgU) printf(" %d",bx);
        break;
      case iAsBx:
        printf("%d %d",a,sbx);
        break;
      case iAx:
        printf("%d",MYK(ax));
        break;
      }
      switch (o)
      {
      case OP_LOADK:
        printf("\t; "); PrintConstant(cl->p,bx);
        break;
      case OP_GETUPVAL:
      case OP_SETUPVAL:
        printf("\t; %s",UPVALNAME(b));
        break;
      case OP_GETTABUP:
        printf("\t; %s",UPVALNAME(b));
        if (ISK(c)) { printf(" "); PrintConstant(cl->p,INDEXK(c)); }
        break;
      case OP_SETTABUP:
        printf("\t; %s",UPVALNAME(a));
        if (ISK(b)) { printf(" "); PrintConstant(cl->p,INDEXK(b)); }
        if (ISK(c)) { printf(" "); PrintConstant(cl->p,INDEXK(c)); }
        break;
      case OP_GETTABLE:
      case OP_SELF:
        if (ISK(c)) { printf("\t; "); PrintConstant(cl->p,INDEXK(c)); }
        break;
      case OP_SETTABLE:
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV:
      case OP_POW:
      case OP_EQ:
      case OP_LT:
      case OP_LE:
        if (ISK(b) || ISK(c))
        {
        printf("\t; ");
        if (ISK(b)) PrintConstant(cl->p,INDEXK(b)); else printf("-");
        printf(" ");
        if (ISK(c)) PrintConstant(cl->p,INDEXK(c)); else printf("-");
        }
        break;
      case OP_JMP:
      case OP_FORLOOP:
      case OP_FORPREP:
      case OP_TFORLOOP:
        printf("\t; to %d",sbx+pc+2);
        break;
      case OP_CLOSURE:
        printf("\t; %p",VOID(cl->p->p[bx]));
        break;
      case OP_SETLIST:
        if (c==0) printf("\t; %d",(int)cl->p->code[pc - cl->p->code + 1]); else printf("\t; %d",c);
        break;
      case OP_EXTRAARG:
        printf("\t; "); PrintConstant(cl->p,ax);
        break;
      default:
        break;
      }
      printf("\n");
      debugend:
    }
#endif
    vmdispatch (GET_OPCODE(i)) {
      vmcase(OP_MOVE,
        setobjs2s(L, ra, RB(i));
      )
      vmcase(OP_LOADK,
        TValue *rb = k + GETARG_Bx(i);
        setobj2s(L, ra, rb);
      )
      vmcase(OP_LOADKX,
        TValue *rb;
        lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
        rb = k + GETARG_Ax(*ci->u.l.savedpc++);
        setobj2s(L, ra, rb);
      )
      vmcase(OP_LOADBOOL,
        setbvalue(ra, GETARG_B(i));
        if (GETARG_C(i)) ci->u.l.savedpc++;  /* skip next instruction (if C) */
      )
      vmcase(OP_LOADNIL,
        int b = GETARG_B(i);
        do {
          setnilvalue(ra++);
        } while (b--);
      )
      vmcase(OP_GETUPVAL,
        int b = GETARG_B(i);
        setobj2s(L, ra, cl->upvals[b]->v);
      )
      vmcase(OP_GETTABUP,
        int b = GETARG_B(i);
        Protect(luaV_gettable(L, cl->upvals[b]->v, RKC(i), ra));
      )
      vmcase(OP_GETTABLE,
        Protect(luaV_gettable(L, RB(i), RKC(i), ra));
      )
      vmcase(OP_SETTABUP,
        int a = GETARG_A(i);
        Protect(luaV_settable(L, cl->upvals[a]->v, RKB(i), RKC(i)));
      )
      vmcase(OP_SETUPVAL,
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_barrier(L, uv, ra);
      )
      vmcase(OP_SETTABLE,
        Protect(luaV_settable(L, ra, RKB(i), RKC(i)));
      )
      vmcase(OP_NEWTABLE,
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        Table *t = luaH_new(L);
        sethvalue(L, ra, t);
        if (b != 0 || c != 0)
          luaH_resize(L, t, luaO_fb2int(b), luaO_fb2int(c));
        checkGC(L, ra + 1);
      )
      vmcase(OP_SELF,
        StkId rb = RB(i);
        setobjs2s(L, ra+1, rb);
        Protect(luaV_gettable(L, rb, RKC(i), ra));
      )
      vmcase(OP_ADD,
        arith_op(luai_numadd, TM_ADD);
      )
      vmcase(OP_SUB,
        arith_op(luai_numsub, TM_SUB);
      )
      vmcase(OP_MUL,
        arith_op(luai_nummul, TM_MUL);
      )
      vmcase(OP_DIV,
        arith_op(luai_numdiv, TM_DIV);
      )
      vmcase(OP_MOD,
        arith_op(luai_nummod, TM_MOD);
      )
      vmcase(OP_POW,
        arith_op(luai_numpow, TM_POW);
      )
      vmcase(OP_UNM,
        TValue *rb = RB(i);
        if (ttisnumber(rb)) {
          lua_Number nb = nvalue(rb);
          setnvalue(ra, luai_numunm(L, nb));
        }
        else {
          Protect(luaV_arith(L, ra, rb, rb, TM_UNM));
        }
      )
      vmcase(OP_NOT,
        TValue *rb = RB(i);
        int res = l_isfalse(rb);  /* next assignment may change this value */
        setbvalue(ra, res);
      )
      vmcase(OP_LEN,
        Protect(luaV_objlen(L, ra, RB(i)));
      )
      vmcase(OP_CONCAT,
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        StkId rb;
        L->top = base + c + 1;  /* mark the end of concat operands */
        Protect(luaV_concat(L, c - b + 1));
        ra = RA(i);  /* 'luav_concat' may invoke TMs and move the stack */
        rb = b + base;
        setobjs2s(L, ra, rb);
        checkGC(L, (ra >= rb ? ra + 1 : rb));
        L->top = ci->top;  /* restore top */
      )
      vmcase(OP_JMP,
        dojump(ci, i, 0);
      )
      vmcase(OP_EQ,
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        resolverope(L, rb);
        resolvesubstr(L, rb);
        resolverope(L, rc);
        resolvesubstr(L, rc);
        Protect(
          if (cast_int(equalobj(L, rb, rc)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            donextjump(ci);
        )
      )
      vmcase(OP_LT,
        Protect(
          if (luaV_lessthan(L, RKB(i), RKC(i)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            donextjump(ci);
        )
      )
      vmcase(OP_LE,
        Protect(
          if (luaV_lessequal(L, RKB(i), RKC(i)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            donextjump(ci);
        )
      )
      vmcase(OP_TEST,
        if (GETARG_C(i) ? l_isfalse(ra) : !l_isfalse(ra))
            ci->u.l.savedpc++;
          else
          donextjump(ci);
      )
      vmcase(OP_TESTSET,
        TValue *rb = RB(i);
        if (GETARG_C(i) ? l_isfalse(rb) : !l_isfalse(rb))
          ci->u.l.savedpc++;
        else {
          setobjs2s(L, ra, rb);
          donextjump(ci);
        }
      )
      vmcase(OP_CALL,
        int b = GETARG_B(i);
        int nresults = GETARG_C(i) - 1;
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        if (luaD_precall(L, ra, nresults)) {  /* C function? */
          if (nresults >= 0) L->top = ci->top;  /* adjust results */
          base = ci->u.l.base;
        }
        else {  /* Lua function */
          ci = L->ci;
          ci->callstatus |= CIST_REENTRY;
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
      )
      vmcase(OP_TAILCALL,
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        lua_assert(GETARG_C(i) - 1 == LUA_MULTRET);
        if (luaD_precall(L, ra, LUA_MULTRET))  /* C function? */
          base = ci->u.l.base;
        else {
          /* tail call: put called frame (n) in place of caller one (o) */
          CallInfo *nci = L->ci;  /* called frame */
          CallInfo *oci = nci->previous;  /* caller frame */
          StkId nfunc = nci->func;  /* called function */
          StkId ofunc = oci->func;  /* caller function */
          /* last stack slot filled by 'precall' */
          StkId lim = nci->u.l.base + getproto(nfunc)->numparams;
          int aux;
          /* close all upvalues from previous call */
          if (cl->p->sizep > 0) luaF_close(L, oci->u.l.base);
          /* move new frame into old one */
          for (aux = 0; nfunc + aux < lim; aux++)
            setobjs2s(L, ofunc + aux, nfunc + aux);
          oci->u.l.base = ofunc + (nci->u.l.base - nfunc);  /* correct base */
          oci->top = L->top = ofunc + (L->top - nfunc);  /* correct top */
          oci->u.l.savedpc = nci->u.l.savedpc;
          oci->callstatus |= CIST_TAIL;  /* function was tail called */
          ci = L->ci = oci;  /* remove new frame */
          lua_assert(L->top == oci->u.l.base + getproto(ofunc)->maxstacksize);
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
      )
      vmcasenb(OP_RETURN,
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b-1;
        if (cl->p->sizep > 0) luaF_close(L, base);
        b = luaD_poscall(L, ra);
        if (!(ci->callstatus & CIST_REENTRY))  /* 'ci' still the called one */
          return;  /* external invocation: return */
        else {  /* invocation via reentry: continue execution */
          ci = L->ci;
          if (b) L->top = ci->top;
          lua_assert(isLua(ci));
          lua_assert(GET_OPCODE(*((ci)->u.l.savedpc - 1)) == OP_CALL);
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
      )
      vmcase(OP_FORLOOP,
        lua_Number step = nvalue(ra+2);
        lua_Number idx = luai_numadd(L, nvalue(ra), step); /* increment index */
        lua_Number limit = nvalue(ra+1);
        if (luai_numlt(L, 0, step) ? luai_numle(L, idx, limit)
                                   : luai_numle(L, limit, idx)) {
          ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
          setnvalue(ra, idx);  /* update internal index... */
          setnvalue(ra+3, idx);  /* ...and external index */
        }
      )
      vmcase(OP_FORPREP,
        const TValue *init = ra;
        const TValue *plimit = ra+1;
        const TValue *pstep = ra+2;
        if (!tonumber(L, init, ra))
          luaG_runerror(L, LUA_QL("for") " initial value must be a number");
        else if (!tonumber(L, plimit, ra+1))
          luaG_runerror(L, LUA_QL("for") " limit must be a number");
        else if (!tonumber(L, pstep, ra+2))
          luaG_runerror(L, LUA_QL("for") " step must be a number");
        setnvalue(ra, luai_numsub(L, nvalue(ra), nvalue(pstep)));
        ci->u.l.savedpc += GETARG_sBx(i);
      )
      vmcasenb(OP_TFORCALL,
        StkId cb = ra + 3;  /* call base */
        setobjs2s(L, cb+2, ra+2);
        setobjs2s(L, cb+1, ra+1);
        setobjs2s(L, cb, ra);
        L->top = cb + 3;  /* func. + 2 args (state and index) */
        Protect(luaD_call(L, cb, GETARG_C(i), 1));
        L->top = ci->top;
        i = *(ci->u.l.savedpc++);  /* go to next instruction */
        ra = RA(i);
        lua_assert(GET_OPCODE(i) == OP_TFORLOOP);
        goto l_tforloop;
      )
      vmcase(OP_TFORLOOP,
        l_tforloop:
        if (!ttisnil(ra + 1)) {  /* continue loop? */
          setobjs2s(L, ra, ra + 1);  /* save control variable */
           ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
        }
      )
      vmcase(OP_SETLIST,
        int n = GETARG_B(i);
        int c = GETARG_C(i);
        int last;
        Table *h;
        if (n == 0) n = cast_int(L->top - ra) - 1;
        if (c == 0) {
          lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
          c = GETARG_Ax(*ci->u.l.savedpc++);
        }
        luai_runtimecheck(L, ttistable(ra));
        h = hvalue(ra);
        last = ((c-1)*LFIELDS_PER_FLUSH) + n;
        if (last > h->sizearray)  /* needs more space? */
          luaH_resizearray(L, h, last);  /* pre-allocate it at once */
        for (; n > 0; n--) {
          TValue *val = ra+n;
          luaH_setint(L, h, last--, val);
          luaC_barrierback(L, obj2gco(h), val);
        }
        L->top = ci->top;  /* correct top (in case of previous open call) */
      )
      vmcase(OP_CLOSURE,
        Proto *p = cl->p->p[GETARG_Bx(i)];
        Closure *ncl = getcached(p, cl->upvals, base);  /* cached closure */
        if (ncl == NULL)  /* no match? */
          pushclosure(L, p, cl->upvals, base, ra);  /* create a new one */
        else
          setclLvalue(L, ra, ncl);  /* push cashed closure */
        checkGC(L, ra + 1);
      )
      vmcase(OP_VARARG,
        int b = GETARG_B(i) - 1;
        int j;
        int n = cast_int(base - ci->func) - cl->p->numparams - 1;
        if (b < 0) {  /* B == 0? */
          b = n;  /* get all var. arguments */
          Protect(luaD_checkstack(L, n));
          ra = RA(i);  /* previous call may change the stack */
          L->top = ra + n;
        }
        for (j = 0; j < b; j++) {
          if (j < n) {
            setobjs2s(L, ra + j, base - n + j);
          }
          else {
            setnilvalue(ra + j);
          }
        }
      )
      vmcase(OP_EXTRAARG,
        lua_assert(0);
      )
    }
  }
}

