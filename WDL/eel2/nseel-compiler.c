/*
  Expression Evaluator Library (NS-EEL) v2
  Copyright (C) 2004-2008 Cockos Incorporated
  Copyright (C) 1999-2003 Nullsoft, Inc.
  
  nseel-compiler.c

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/


// for VirtualProtect

#include "ns-eel-int.h"

#include "../denormal.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>

#ifndef _WIN64
  #ifndef __ppc__
    #include <float.h>
  #endif
#endif

#ifdef __APPLE__
  #ifdef __LP64__
    #define EEL_USE_MPROTECT
  #endif
#endif

#ifdef EEL_USE_MPROTECT
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#endif

#ifdef NSEEL_EEL1_COMPAT_MODE

#ifndef EEL_NO_CHANGE_FPFLAGS
#define EEL_NO_CHANGE_FPFLAGS
#endif

#endif

#define EEL_STACK_SUPPORT



//#define LOG_OPT
//#define EEL_PPC_NOFREECODE

#ifndef _WIN64
#if !defined(_RC_CHOP) && !defined(EEL_NO_CHANGE_FPFLAGS)

#include <fpu_control.h>
#define _RC_CHOP _FPU_RC_ZERO
#define _MCW_RC _FPU_RC_ZERO
static unsigned int _controlfp(unsigned int val, unsigned int mask)
{
   unsigned int ret;
   _FPU_GETCW(ret);
   if (mask)
   {
     ret&=~mask;
     ret|=val;
     _FPU_SETCW(ret);
   }
   return ret;
}

#endif
#endif

/*
  P1 is rightmost parameter
  P2 is second rightmost, if any
  P3 is third rightmost, if any
  registers on x86 are  (RAX etc on x86-64)
    P1(ret) EAX
    P2 EDI
    P3 ECX
    WTP RSI

  registers on PPC are:
    P1(ret) r3
    P2 r14 
    P3 r15
    WTP r16 (r17 has the original value)

  */

#ifdef __ppc__

#define GLUE_MOV_PX_DIRECTVALUE_SIZE 8
static void GLUE_MOV_PX_DIRECTVALUE_GEN(void *b, INT_PTR v, int wv) 
{   
  static const unsigned short tab[3][2] = {
    {0x3C60, 0x6063}, // addis r3, r0, hw -- ori r3,r3, lw
    {0x3DC0, 0x61CE}, // addis r14, r0, hw -- ori r14, r14, lw
    {0x3DE0, 0x61EF}, // addis r15, r0, hw -- oris r15, r15, lw
  };
  unsigned int uv=(unsigned int)v;
	unsigned short *p=(unsigned short *)b;

  *p++ = tab[wv][0]; // addis rX, r0, hw
	*p++ = (uv>>16)&0xffff;
  *p++ = tab[wv][1]; // ori rX, rX, lw
	*p++ = uv&0xffff;
}


// mflr r5
// stwu r5, -4(r1)
const static unsigned int GLUE_FUNC_ENTER[2] = { 0x7CA802A6, 0x94A1FFFC };

// lwz r5, 0(r1)
// addi r1, r1, 4
// mtlr r5
const static unsigned int GLUE_FUNC_LEAVE[3] = { 0x80A10000, 0x38210004, 0x7CA803A6 };
#define GLUE_FUNC_ENTER_SIZE sizeof(GLUE_FUNC_ENTER)
#define GLUE_FUNC_LEAVE_SIZE sizeof(GLUE_FUNC_LEAVE)

const static unsigned int GLUE_RET[]={0x4E800020}; // blr

static int GLUE_RESET_WTP(unsigned char *out, void *ptr)
{
  const static unsigned int GLUE_SET_WTP_FROM_R17=0x7E308B78; // mr r16 (dest), r17 (src)
  if (out) memcpy(out,&GLUE_SET_WTP_FROM_R17,sizeof(GLUE_SET_WTP_FROM_R17));
  return sizeof(GLUE_SET_WTP_FROM_R17);

}



// stwu r3, -4(r1)
const static unsigned int GLUE_PUSH_P1[1]={ 0x9461FFFC};


#define GLUE_POP_PX_SIZE 8
static void GLUE_POP_PX(void *b, int wv)
{
  static const unsigned int tab[3] ={
      0x80610000, // lwz r3, 0(r1)
      0x81c10000, // lwz r14, 0(r1)
      0x81e10000, // lwz r15, 0(r1)
  };
  ((unsigned int *)b)[0] = tab[wv];
  ((unsigned int *)b)[1] = 0x38210004; // addi r1,r1, 4
}

#define GLUE_SET_PX_FROM_P1_SIZE 4
static void GLUE_SET_PX_FROM_P1(void *b, int wv)
{
  static const unsigned int tab[3]={
    0x7c631b78, // never used: mr r3, r3
    0x7c6e1b78, // mr r14, r3
    0x7c6f1b78, // mr r15, r3
  };
  *(unsigned int *)b  = tab[wv];
}



// lwz r14, 0(r3)
// lwz r15, 4(r3)
// stwu r15, -4(r1)
// stwu r14, -4(r1)
static unsigned int GLUE_PUSH_P1PTR_AS_VALUE[] = { 0x81C30000, 0x81E30004, 0x95E1FFFC, 0x95C1FFFC,  };

static int GLUE_POP_VALUE_TO_ADDR(unsigned char *buf, void *destptr)
{    
  // lwz r14, 0(r1)
  // lwz r15, 4(r1)
  // addi r1,r1,8
  // GLUE_MOV_PX_DIRECTVALUE_GEN / GLUE_MOV_PX_DIRECTVALUE_SIZE (r3)
  // stw r14, 0(r3)
  // stw r15, 4(r3)
  if (buf)
  {
    unsigned int *bufptr = (unsigned int *)buf;
    *bufptr++ = 0x81C10000;
    *bufptr++ = 0x81E10004;
    *bufptr++ = 0x38210008;    
    GLUE_MOV_PX_DIRECTVALUE_GEN(bufptr, (unsigned int)destptr,0);
    bufptr += GLUE_MOV_PX_DIRECTVALUE_SIZE/4;
    *bufptr++ = 0x95C30000;
    *bufptr++ = 0x95E30004;
  }
  return 3*4 + GLUE_MOV_PX_DIRECTVALUE_SIZE + 2*4;
}

static int GLUE_COPY_VALUE_AT_P1_TO_PTR(unsigned char *buf, void *destptr)
{    
  // lwz r14, 0(r3)
  // lwz r15, 4(r3)
  // GLUE_MOV_PX_DIRECTVALUE_GEN / GLUE_MOV_PX_DIRECTVALUE_SIZE (r3)
  // stw r14, 0(r3)
  // stw r15, 4(r3)

  if (buf)
  {
    unsigned int *bufptr = (unsigned int *)buf;
    *bufptr++ = 0x81C30000;
    *bufptr++ = 0x81E30004;
    GLUE_MOV_PX_DIRECTVALUE_GEN(bufptr, (unsigned int)destptr,0);
    bufptr += GLUE_MOV_PX_DIRECTVALUE_SIZE/4;
    *bufptr++ = 0x95C30000;
    *bufptr++ = 0x95E30004;
  }
  
  return 2*4 + GLUE_MOV_PX_DIRECTVALUE_SIZE + 2*4;
}



static void GLUE_CALL_CODE(INT_PTR bp, INT_PTR cp) 
{
  __asm__(
          "stmw r14, -80(r1)\n"
          "mtctr %0\n"
          "mr r17, %1\n" 
	  "subi r17, r17, 8\n"
          "mflr r5\n" 
          "stw r5, -84(r1)\n"
          "subi r1, r1, 88\n"
          "bctrl\n"
          "addi r1, r1, 88\n"
          "lwz r5, -84(r1)\n"
          "lmw r14, -80(r1)\n"
          "mtlr r5\n"
            ::"r" (cp), "r" (bp));
};

unsigned char *EEL_GLUE_set_immediate(void *_p, const void *newv)
{
  // 64 bit ppc would take some work
  unsigned int *p=(unsigned int *)_p;
  while ((p[0]&0x0000FFFF) != 0x0000dead && 
         (p[1]&0x0000FFFF) != 0x0000beef) p++;
  p[0] = (p[0]&0xFFFF0000) | (((unsigned int)newv)>>16);
  p[1] = (p[1]&0xFFFF0000) | (((unsigned int)newv)&0xFFFF);

  return (unsigned char *)(p+1);
}

#else 

//x86 and x86-64 code

#define GLUE_FUNC_ENTER_SIZE 0
#define GLUE_FUNC_LEAVE_SIZE 0
const static unsigned int GLUE_FUNC_ENTER[1];
const static unsigned int GLUE_FUNC_LEAVE[1];

#if defined(_WIN64) || defined(__LP64__)
  #define GLUE_MOV_PX_DIRECTVALUE_SIZE 10
  static void GLUE_MOV_PX_DIRECTVALUE_GEN(void *b, INT_PTR v, int wr) {   
    const static unsigned short tab[3] = 
    {
      0xB848 /* mov rax, dv*/, 
      0xBF48 /* mov rdi, dv */ , 
      0xB948 /* mov rcx, dv */ 
    };
    unsigned short *bb = (unsigned short *)b;
    *bb++ = tab[wr];  // mov rax, directvalue
    *(INT_PTR *)bb = v; 
  }

  const static unsigned char  GLUE_PUSH_P1[2]={	   0x50,0x50}; // push rax ; push rax (push twice to preserve alignment)

  #define GLUE_POP_PX_SIZE 2
  static void GLUE_POP_PX(void *b, int wv)
  {
    static const unsigned char tab[3][GLUE_POP_PX_SIZE]=
    {
      {0x58,/*pop eax*/  0x58}, // pop twice to preserve alignment
      {0x5F,/*pop edi*/  0x5F}, 
      {0x59,/*pop ecx*/  0x59}, 
    };    
    memcpy(b,tab[wv],GLUE_POP_PX_SIZE);
  }

  static unsigned char GLUE_PUSH_P1PTR_AS_VALUE[] = {  0xff, 0x30 /* push qword [rax] */, 0x50 /*push rax - for alignment */ };
  static int GLUE_POP_VALUE_TO_ADDR(unsigned char *buf, void *destptr)
  {    
    if (buf)
    {
      *buf++ = 0x48; *buf++ = 0xB9; *(void **) buf = destptr; buf+=8; // mov rcx, directvalue
      *buf++ = 0x5F ; // pop rdi (alignment)
      *buf++ = 0x8f; *buf++ = 0x01; // pop qword [rcx]      
    }
    return 1+10+2;
  }

  static int GLUE_COPY_VALUE_AT_P1_TO_PTR(unsigned char *buf, void *destptr)
  {    
    if (buf)
    {
      *buf++ = 0x48; *buf++ = 0xB9; *(void **) buf = destptr; buf+=8; // mov rcx, directvalue
      *buf++ = 0x48; *buf++ = 0x8B; *buf++ = 0x38; // mov rdi, [rax]
      *buf++ = 0x48; *buf++ = 0x89; *buf++ = 0x39; // mov [rcx], rdi
    }

    return 3 + 10 + 3;
  }

  #define GLUE_SET_PX_FROM_P1_SIZE 3
  static void GLUE_SET_PX_FROM_P1(void *b, int wv)
  {
    static const unsigned char tab[3][GLUE_SET_PX_FROM_P1_SIZE]={
      {0x90,0x90,0x90}, // should never be used! (nopnop)
      {0x48,0x89,0xC7}, // mov edi, eax
      {0x48,0x89,0xC1}, // mov ecx, eax
    };
    memcpy(b,tab[wv],GLUE_SET_PX_FROM_P1_SIZE);
  }
#else

  static unsigned char GLUE_PUSH_P1PTR_AS_VALUE[] = { 0x83, 0xEC, 8, /* sub esp, 8 */   0xff, 0x30 /* push dword [eax] */, 0xff, 0x70, 0x4 /* push dword [eax+4] */ };

  static int GLUE_POP_VALUE_TO_ADDR(unsigned char *buf, void *destptr)
  {    
    if (buf)
    {
      *buf++ = 0xB8; *(void **) buf = destptr; buf+=4; // mov eax, directvalue
      
      *buf++ = 0x8f; *buf++ = 0x40; *buf++ = 4; // pop dword [eax+4]
      
      *buf++ = 0x8f; *buf++ = 0x00; // pop dword [eax]
      
      *buf++ = 0x59; // pop ecx
      *buf++ = 0x59; // pop ecx
    }
    
    return 12;
  }

  static int GLUE_COPY_VALUE_AT_P1_TO_PTR(unsigned char *buf, void *destptr)
  {    
    if (buf)
    {
      *buf++ = 0x8B; *buf++ = 0x38; // mov edi, [eax]
      *buf++ = 0x8B; *buf++ = 0x48; *buf++ = 0x04; // mov ecx, [eax+4]
      
      
      *buf++ = 0xB8; *(void **) buf = destptr; buf+=4; // mov eax, directvalue
      *buf++ = 0x89; *buf++ = 0x38; // mov [eax], edi
      *buf++ = 0x89; *buf++ = 0x48; *buf++ = 0x04; // mov [eax+4], ecx
    }
    
    return 2 + 3 + 5 + 2 + 3;
  }


  #define GLUE_MOV_PX_DIRECTVALUE_SIZE 5
  static void GLUE_MOV_PX_DIRECTVALUE_GEN(void *b, int v, int wv) 
  {   
    const static unsigned char tab[3] = {
      0xB8 /* mov eax, dv*/, 
      0xBF /* mov edi, dv */ , 
      0xB9 /* mov ecx, dv */ 
    };
    *((unsigned char *)b) = tab[wv];  // mov eax, dv
    b= ((unsigned char *)b)+1;
    *(int *)b = v; 
  }
  const static unsigned char  GLUE_PUSH_P1[4]={0x83, 0xEC, 12,   0x50}; // sub esp, 12, push eax
  
  #define GLUE_POP_PX_SIZE 4
  static void GLUE_POP_PX(void *b, int wv)
  {
    static const unsigned char tab[3][GLUE_POP_PX_SIZE]=
    {
      {0x58,/*pop eax*/  0x83, 0xC4, 12 /* add esp, 12*/},
      {0x5F,/*pop edi*/  0x83, 0xC4, 12}, 
      {0x59,/*pop ecx*/  0x83, 0xC4, 12}, 
    };    
    memcpy(b,tab[wv],GLUE_POP_PX_SIZE);
  }

  #define GLUE_SET_PX_FROM_P1_SIZE 2
  static void GLUE_SET_PX_FROM_P1(void *b, int wv)
  {
    static const unsigned char tab[3][GLUE_SET_PX_FROM_P1_SIZE]={
      {0x90,0x90}, // should never be used! (nopnop)
      {0x89,0xC7}, // mov edi, eax
      {0x89,0xC1}, // mov ecx, eax
    };
    memcpy(b,tab[wv],GLUE_SET_PX_FROM_P1_SIZE);
  }

#endif


const static unsigned char  GLUE_RET=0xC3;

static int GLUE_RESET_WTP(unsigned char *out, void *ptr)
{
#if defined(_WIN64) || defined(__LP64__)
  if (out)
  {
	  *out++ = 0x48;
    *out++ = 0xBE; // mov rsi, constant64
  	*(void **)out = ptr;
    out+=sizeof(void *);
  }
  return 2+sizeof(void *);
#else
  if (out)
  {
    *out++ = 0xBE; // mov esi, constant
    memcpy(out,&ptr,sizeof(void *));
    out+=sizeof(void *);
  }
  return 1+sizeof(void *);
#endif
}


static void GLUE_CALL_CODE(INT_PTR bp, INT_PTR cp) 
{
  #if defined(_WIN64) || defined(__LP64__)
    extern void win64_callcode(INT_PTR code);
    win64_callcode(cp);
  #else // non-64 bit
    #ifdef _MSC_VER
      #ifndef EEL_NO_CHANGE_FPFLAGS
        unsigned int old_v=_controlfp(0,0); 
        _controlfp(_RC_CHOP,_MCW_RC);
      #endif

      __asm
      {
        mov eax, cp
        pushad 
        call eax
        popad
      };

      #ifndef EEL_NO_CHANGE_FPFLAGS
        _controlfp(old_v,_MCW_RC);
      #endif
    #else // gcc x86
      #ifndef EEL_NO_CHANGE_FPFLAGS
        unsigned int old_v=_controlfp(0,0); 
        _controlfp(_RC_CHOP,_MCW_RC);
      #endif
      __asm__("call *%%eax"::"a" (cp): "%ecx","%edx","%esi","%edi");
      #ifndef EEL_NO_CHANGE_FPFLAGS
        _controlfp(old_v,_MCW_RC);
      #endif
    #endif //gcc x86
  #endif // 32bit
}

unsigned char *EEL_GLUE_set_immediate(void *_p, const void *newv)
{
  char *p=(char*)_p;
#if defined(__LP64__) || defined(_WIN64)
  INT_PTR scan = 0xFEFEFEFEFEFEFEFE;
#else
  INT_PTR scan = 0xFEFEFEFE;
#endif
  while (*(INT_PTR *)p != scan) p++;
  *(INT_PTR *)p = (INT_PTR)newv;
  return (unsigned char *) (((INT_PTR*)p)+1);
}




#endif

static void *GLUE_realAddress(void *fn, void *fn_e, int *size)
{
#if defined(_MSC_VER) || defined(__LP64__)

  unsigned char *p;

#if defined(_DEBUG) && !defined(__LP64__)
  if (*(unsigned char *)fn == 0xE9) // this means jump to the following address
  {
    fn = ((unsigned char *)fn) + *(int *)((char *)fn+1) + 5;
  }
#endif

  // this may not work in debug mode?
  p=(unsigned char *)fn;
  for (;;)
  {
    int a;
    for (a=0;a<12;a++)
    {
      if (p[a] != (a?0x90:0x89)) break;
    }
    if (a>=12)
    {
      *size = (char *)p - (char *)fn;
    //  if (*size<0) MessageBox(NULL,"expect poof","a",0);
      return fn;
    }
    p++;
  }
#else

  char *p=(char *)fn_e - sizeof(GLUE_RET);
  if (p <= (char *)fn) *size=0;
  else
  {
    while (p > (char *)fn && memcmp(p,&GLUE_RET,sizeof(GLUE_RET))) p-=sizeof(GLUE_RET);
    *size = p - (char *)fn;
  }
  return fn;

#endif
}



static int nseel_evallib_stats[5]; // source bytes, static code bytes, call code bytes, data bytes, segments
int *NSEEL_getstats()
{
  return nseel_evallib_stats;
}
EEL_F *NSEEL_getglobalregs()
{
  return nseel_globalregs;
}

// this stuff almost works
static int findByteOffsetInSource(compileContext *ctx, int byteoffs,int *destoffs)
{
	int x;
	if (!ctx->compileLineRecs || !ctx->compileLineRecs_size) return *destoffs=0;
	if (byteoffs < ctx->compileLineRecs[0].destByteCount) 
	{
		*destoffs=0;
		return 1;
	}
	for (x = 0; x < ctx->compileLineRecs_size-1; x ++)
	{
		if (byteoffs >= ctx->compileLineRecs[x].destByteCount &&
		    byteoffs < ctx->compileLineRecs[x+1].destByteCount) break;
	}
	*destoffs=ctx->compileLineRecs[(x&&x==ctx->compileLineRecs_size-1)?x-1:x].srcByteCount;

	return x+2;
}


static void onCompileNewLine(compileContext *ctx, int srcBytes, int destBytes)
{
	if (!ctx->compileLineRecs || ctx->compileLineRecs_size >= ctx->compileLineRecs_alloc)
	{
		ctx->compileLineRecs_alloc = ctx->compileLineRecs_size+1024;
		ctx->compileLineRecs = (lineRecItem *)realloc(ctx->compileLineRecs,sizeof(lineRecItem)*ctx->compileLineRecs_alloc);
	}
	if (ctx->compileLineRecs)
	{
		ctx->compileLineRecs[ctx->compileLineRecs_size].srcByteCount=srcBytes;
		ctx->compileLineRecs[ctx->compileLineRecs_size++].destByteCount=destBytes;
	}
}

static void *__newBlock(llBlock **start,int size, char wantMprotect);

#define OPCODE_IS_TRIVIAL(x) ((x)->opcodeType <= OPCODETYPE_VARPTRPTR)
enum {
  OPCODETYPE_DIRECTVALUE=0,
  OPCODETYPE_VALUE_FROM_NAMESPACENAME, // this.* are encoded this way
  OPCODETYPE_VARPTR,
  OPCODETYPE_VARPTRPTR,
  OPCODETYPE_FUNC1,
  OPCODETYPE_FUNC2,
  OPCODETYPE_FUNC3,

  OPCODETYPE_INVALID,
};

struct opcodeRec
{
 int opcodeType; 
 int fntype;
 void *fn;
 
 union {
   struct opcodeRec *parms[3];
   struct {
     double directValue;
     EEL_F *valuePtr; // if direct value, valuePtr can be cached
   } dv;
 } parms;
  
 // allocate extra if using this field. used with:
 // OPCODETYPE_VALUE_FROM_NAMESPACENAME
 ///  or 
 // OPCODETYPE_FUNC* with fntype=FUNCTYPE_EELFUNC_THIS
 char relname[1]; 
};




static void *newTmpBlock(compileContext *ctx, int size)
{
  const int align = 8;
  const int a1=align-1;
  char *p=(char*)__newBlock(&ctx->tmpblocks_head,size+a1, 0);
  return p+((align-(((INT_PTR)p)&a1))&a1);
}

static void *__newBlock_align(compileContext *ctx, int size, int align, char isForCode) 
{
  const int a1=align-1;
  char *p=(char*)__newBlock(
                            (isForCode < 0 ? &ctx->tmpblocks_head : 
                                         isForCode > 0 ? &ctx->blocks_head : 
                                          &ctx->blocks_head_data) ,size+a1, isForCode>0);
  return p+((align-(((INT_PTR)p)&a1))&a1);
}

static opcodeRec *newOpCode(compileContext *ctx)
{
  return (opcodeRec*)__newBlock_align(ctx,sizeof(opcodeRec),8, ctx->isSharedFunctions ? 0 : -1); 
}

#define newCodeBlock(x,a) __newBlock_align(ctx,x,a,1)
#define newDataBlock(x,a) __newBlock_align(ctx,x,a,0)


static void freeBlocks(llBlock **start);

#define DECL_ASMFUNC(x)         \
  void nseel_asm_##x(void);        \
  void nseel_asm_##x##_end(void);    \

  DECL_ASMFUNC(sin)
  DECL_ASMFUNC(cos)
  DECL_ASMFUNC(tan)
  DECL_ASMFUNC(1pdd)
  DECL_ASMFUNC(2pdd)
  DECL_ASMFUNC(2pdds)
  DECL_ASMFUNC(1pp)
  DECL_ASMFUNC(2pp)
  DECL_ASMFUNC(sqr)
  DECL_ASMFUNC(sqrt)
  DECL_ASMFUNC(log)
  DECL_ASMFUNC(log10)
  DECL_ASMFUNC(abs)
  DECL_ASMFUNC(min)
  DECL_ASMFUNC(max)
  DECL_ASMFUNC(sig)
  DECL_ASMFUNC(sign)
  DECL_ASMFUNC(band)
  DECL_ASMFUNC(bor)
  DECL_ASMFUNC(bnot)
  DECL_ASMFUNC(if)
  DECL_ASMFUNC(fcall)
  DECL_ASMFUNC(repeat)
  DECL_ASMFUNC(repeatwhile)
  DECL_ASMFUNC(equal)
  DECL_ASMFUNC(notequal)
  DECL_ASMFUNC(below)
  DECL_ASMFUNC(above)
  DECL_ASMFUNC(beloweq)
  DECL_ASMFUNC(aboveeq)
  DECL_ASMFUNC(assign)
  DECL_ASMFUNC(assign_fast)
  DECL_ASMFUNC(add)
  DECL_ASMFUNC(sub)
  DECL_ASMFUNC(add_op)
  DECL_ASMFUNC(sub_op)
  DECL_ASMFUNC(mul)
  DECL_ASMFUNC(div)
  DECL_ASMFUNC(mul_op)
  DECL_ASMFUNC(div_op)
  DECL_ASMFUNC(mod)
  DECL_ASMFUNC(shl)
  DECL_ASMFUNC(shr)
  DECL_ASMFUNC(mod_op)
  DECL_ASMFUNC(or)
  DECL_ASMFUNC(xor)
  DECL_ASMFUNC(xor_op)
  DECL_ASMFUNC(and)
  DECL_ASMFUNC(or_op)
  DECL_ASMFUNC(and_op)
  DECL_ASMFUNC(uplus)
  DECL_ASMFUNC(uminus)
  DECL_ASMFUNC(invsqrt)
  DECL_ASMFUNC(exec2)

#ifdef EEL_STACK_SUPPORT
  DECL_ASMFUNC(stack_push)
  DECL_ASMFUNC(stack_pop)
  DECL_ASMFUNC(stack_pop_fast) // just returns value, doesn't mod param
  DECL_ASMFUNC(stack_peek)
  DECL_ASMFUNC(stack_peek_int)
  DECL_ASMFUNC(stack_peek_top)
  DECL_ASMFUNC(stack_exch)
#endif

static void NSEEL_PProc_GRAM(void *data, int data_size, compileContext *ctx)
{
  if (data_size>0) EEL_GLUE_set_immediate(data, ctx->gram_blocks);
}

static void NSEEL_PProc_Stack(void *data, int data_size, compileContext *ctx)
{
  codeHandleType *ch=ctx->tmpCodeHandle;

  if (data_size>0) 
  {
    UINT_PTR m1=(UINT_PTR)(NSEEL_STACK_SIZE * sizeof(EEL_F) - 1);
    UINT_PTR stackptr = ((UINT_PTR) (&ch->stack));

    ch->want_stack=1;
    if (!ch->stack) ch->stack = newDataBlock(NSEEL_STACK_SIZE*sizeof(EEL_F),NSEEL_STACK_SIZE*sizeof(EEL_F));

    data=EEL_GLUE_set_immediate(data, (void *)stackptr);
    data=EEL_GLUE_set_immediate(data, (void*) m1); // and
    data=EEL_GLUE_set_immediate(data, (void *)((UINT_PTR)ch->stack&~m1)); //or
  }
}

static void NSEEL_PProc_Stack_PeekInt(void *data, int data_size, compileContext *ctx, INT_PTR offs)
{
  codeHandleType *ch=ctx->tmpCodeHandle;

  if (data_size>0) 
  {
    UINT_PTR m1=(UINT_PTR)(NSEEL_STACK_SIZE * sizeof(EEL_F) - 1);
    UINT_PTR stackptr = ((UINT_PTR) (&ch->stack));

    ch->want_stack=1;
    if (!ch->stack) ch->stack = newDataBlock(NSEEL_STACK_SIZE*sizeof(EEL_F),NSEEL_STACK_SIZE*sizeof(EEL_F));

    data=EEL_GLUE_set_immediate(data, (void *)stackptr);
    data=EEL_GLUE_set_immediate(data, (void *)offs);
    data=EEL_GLUE_set_immediate(data, (void*) m1); // and
    data=EEL_GLUE_set_immediate(data, (void *)((UINT_PTR)ch->stack&~m1)); //or
  }
}
static void NSEEL_PProc_Stack_PeekTop(void *data, int data_size, compileContext *ctx)
{
  codeHandleType *ch=ctx->tmpCodeHandle;

  if (data_size>0) 
  {
    UINT_PTR stackptr = ((UINT_PTR) (&ch->stack));

    ch->want_stack=1;
    if (!ch->stack) ch->stack = newDataBlock(NSEEL_STACK_SIZE*sizeof(EEL_F),NSEEL_STACK_SIZE*sizeof(EEL_F));

    data=EEL_GLUE_set_immediate(data, (void *)stackptr);
  }
}

#ifndef __ppc__
static EEL_F g_signs[2]={1.0,-1.0};
static EEL_F negativezeropointfive=-0.5f;
static EEL_F onepointfive=1.5f;
#endif
static EEL_F g_closefact = NSEEL_CLOSEFACTOR;
static const EEL_F eel_zero=0.0, eel_one=1.0;

#if defined(_MSC_VER) && _MSC_VER >= 1400
static double __floor(double a) { return floor(a); }
#endif

void NSEEL_PProc_RAM_freeblocks(void *data, int data_size, compileContext *ctx);

#ifdef NSEEL_EEL1_COMPAT_MODE
static double eel1band(double a, double b)
{
  return (fabs(a)>g_closefact && fabs(b) > g_closefact) ? 1.0 : 0.0;
}
static double eel1bor(double a, double b)
{
  return (fabs(a)>g_closefact || fabs(b) > g_closefact) ? 1.0 : 0.0;
}

static double eel1sigmoid(double x, double constraint)
{
  double t = (1+exp(-x * (constraint)));
  return fabs(t)>g_closefact ? 1.0/t : 0;
}

#endif

EEL_F NSEEL_CGEN_CALL nseel_int_rand(EEL_F *f);

static functionType fnTable1[] = {
  { "_if",     nseel_asm_if,nseel_asm_if_end,    3,  {&g_closefact} },
  { "_and",   nseel_asm_band,nseel_asm_band_end,  2 } ,
  { "_or",    nseel_asm_bor,nseel_asm_bor_end,   2 } ,
  { "loop", nseel_asm_repeat,nseel_asm_repeat_end, 2 },
  { "while", nseel_asm_repeatwhile,nseel_asm_repeatwhile_end, 1 },

#ifdef __ppc__
  { "_not",   nseel_asm_bnot,nseel_asm_bnot_end,  1, {&g_closefact,&eel_zero,&eel_one} } ,
  { "_equal",  nseel_asm_equal,nseel_asm_equal_end, 2, {&g_closefact,&eel_zero, &eel_one} },
  { "_noteq",  nseel_asm_notequal,nseel_asm_notequal_end, 2, {&g_closefact,&eel_one,&eel_zero} },
  { "_below",  nseel_asm_below,nseel_asm_below_end, 2, {&eel_zero, &eel_one} },
  { "_above",  nseel_asm_above,nseel_asm_above_end, 2, {&eel_zero, &eel_one}  },
  { "_beleq",  nseel_asm_beloweq,nseel_asm_beloweq_end, 2, {&eel_zero, &eel_one}  },
  { "_aboeq",  nseel_asm_aboveeq,nseel_asm_aboveeq_end, 2, {&eel_zero, &eel_one} },
#else
  { "_not",   nseel_asm_bnot,nseel_asm_bnot_end,  1, {&g_closefact} } ,
  { "_equal",  nseel_asm_equal,nseel_asm_equal_end, 2, {&g_closefact} },
  { "_noteq",  nseel_asm_notequal,nseel_asm_notequal_end, 2, {&g_closefact} },
  { "_below",  nseel_asm_below,nseel_asm_below_end, 2 },
  { "_above",  nseel_asm_above,nseel_asm_above_end, 2 },
  { "_beleq",  nseel_asm_beloweq,nseel_asm_beloweq_end, 2 },
  { "_aboeq",  nseel_asm_aboveeq,nseel_asm_aboveeq_end, 2 },
#endif

  { "_set",nseel_asm_assign,nseel_asm_assign_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_mod",nseel_asm_mod,nseel_asm_mod_end,2},
  { "_shr",nseel_asm_shr,nseel_asm_shr_end,2},
  { "_shl",nseel_asm_shl,nseel_asm_shl_end,2},
  { "_mulop",nseel_asm_mul_op,nseel_asm_mul_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_divop",nseel_asm_div_op,nseel_asm_div_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_orop",nseel_asm_or_op,nseel_asm_or_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_andop",nseel_asm_and_op,nseel_asm_and_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_xorop",nseel_asm_xor_op,nseel_asm_xor_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_addop",nseel_asm_add_op,nseel_asm_add_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_subop",nseel_asm_sub_op,nseel_asm_sub_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},
  { "_modop",nseel_asm_mod_op,nseel_asm_mod_op_end,2 | NSEEL_NPARAMS_FLAG_MODSTUFF},


#ifdef __ppc__
   { "sin",   nseel_asm_1pdd,nseel_asm_1pdd_end,   1, {&sin} },
   { "cos",    nseel_asm_1pdd,nseel_asm_1pdd_end,   1, {&cos} },
   { "tan",    nseel_asm_1pdd,nseel_asm_1pdd_end,   1, {&tan}  },
#else
   { "sin",   nseel_asm_sin,nseel_asm_sin_end,   1 },
   { "cos",    nseel_asm_cos,nseel_asm_cos_end,   1 },
   { "tan",    nseel_asm_tan,nseel_asm_tan_end,   1 },
#endif
   { "asin",   nseel_asm_1pdd,nseel_asm_1pdd_end,  1, {&asin}, },
   { "acos",   nseel_asm_1pdd,nseel_asm_1pdd_end,  1, {&acos}, },
   { "atan",   nseel_asm_1pdd,nseel_asm_1pdd_end,  1, {&atan}, },
   { "atan2",  nseel_asm_2pdd,nseel_asm_2pdd_end, 2, {&atan2}, },
   { "sqr",    nseel_asm_sqr,nseel_asm_sqr_end,   1 },
#ifdef __ppc__
   { "sqrt",   nseel_asm_1pdd,nseel_asm_1pdd_end,  1, {&sqrt}, },
#else
   { "sqrt",   nseel_asm_sqrt,nseel_asm_sqrt_end,  1 },
#endif
   { "pow",    nseel_asm_2pdd,nseel_asm_2pdd_end,   2, {&pow}, },
   { "_powop",    nseel_asm_2pdds,nseel_asm_2pdds_end,   2 | NSEEL_NPARAMS_FLAG_MODSTUFF, {&pow}, },
   { "exp",    nseel_asm_1pdd,nseel_asm_1pdd_end,   1, {&exp}, },
#ifdef __ppc__
   { "log",    nseel_asm_1pdd,nseel_asm_1pdd_end,   1, {&log} },
   { "log10",  nseel_asm_1pdd,nseel_asm_1pdd_end, 1, {&log10} },
#else
   { "log",    nseel_asm_log,nseel_asm_log_end,   1, },
   { "log10",  nseel_asm_log10,nseel_asm_log10_end, 1, },
#endif
   { "abs",    nseel_asm_abs,nseel_asm_abs_end,   1 },
   { "min",    nseel_asm_min,nseel_asm_min_end,   2 },
   { "max",    nseel_asm_max,nseel_asm_max_end,   2 },
#ifdef __ppc__
   { "sign",   nseel_asm_sign,nseel_asm_sign_end,  1, {&eel_zero}} ,
#else
   { "sign",   nseel_asm_sign,nseel_asm_sign_end,  1, {&g_signs}} ,
#endif
	 { "rand",   nseel_asm_1pp,nseel_asm_1pp_end,  1, {&nseel_int_rand}, } ,

#if defined(_MSC_VER) && _MSC_VER >= 1400
   { "floor",  nseel_asm_1pdd,nseel_asm_1pdd_end, 1, {&__floor} },
#else
   { "floor",  nseel_asm_1pdd,nseel_asm_1pdd_end, 1, {&floor} },
#endif
   { "ceil",   nseel_asm_1pdd,nseel_asm_1pdd_end,  1, {&ceil} },
#ifdef __ppc__
   { "invsqrt",   nseel_asm_invsqrt,nseel_asm_invsqrt_end,  1,  },
#else
   { "invsqrt",   nseel_asm_invsqrt,nseel_asm_invsqrt_end,  1, {&negativezeropointfive, &onepointfive} },
#endif

  { "_xor",    nseel_asm_xor,nseel_asm_xor_end,   2 } ,

#ifdef NSEEL_EEL1_COMPAT_MODE
  { "sigmoid", nseel_asm_2pdd,nseel_asm_2pdd_end, 2, {&eel1sigmoid}, },

  // these differ from _and/_or, they always evaluate both...
  { "band",  nseel_asm_2pdd,nseel_asm_2pdd_end, 2, {&eel1band}, },
  { "bor",  nseel_asm_2pdd,nseel_asm_2pdd_end, 2, {&eel1bor}, },

  {"exec2",nseel_asm_exec2,nseel_asm_exec2_end,2},
  {"exec3",nseel_asm_exec2,nseel_asm_exec2_end,3},


#endif // end EEL1 compat



  {"_mem",_asm_megabuf,_asm_megabuf_end,1,{&g_closefact,&__NSEEL_RAMAlloc},NSEEL_PProc_RAM},
  {"_gmem",_asm_gmegabuf,_asm_gmegabuf_end,1,{&g_closefact,&__NSEEL_RAMAllocGMEM},NSEEL_PProc_GRAM},
  {"freembuf",_asm_generic1parm,_asm_generic1parm_end,1 | NSEEL_NPARAMS_FLAG_MODSTUFF,{&__NSEEL_RAM_MemFree},NSEEL_PProc_RAM_freeblocks},
  {"memcpy",_asm_generic3parm,_asm_generic3parm_end,3 | NSEEL_NPARAMS_FLAG_MODSTUFF,{&__NSEEL_RAM_MemCpy},NSEEL_PProc_RAM},
  {"memset",_asm_generic3parm,_asm_generic3parm_end,3 | NSEEL_NPARAMS_FLAG_MODSTUFF,{&__NSEEL_RAM_MemSet},NSEEL_PProc_RAM},

#ifdef EEL_STACK_SUPPORT
  {"stack_push",nseel_asm_stack_push,nseel_asm_stack_push_end,1 | NSEEL_NPARAMS_FLAG_MODSTUFF,{0,},NSEEL_PProc_Stack},
  {"stack_pop",nseel_asm_stack_pop,nseel_asm_stack_pop_end,1 | NSEEL_NPARAMS_FLAG_MODSTUFF,{0,},NSEEL_PProc_Stack},
  {"stack_peek",nseel_asm_stack_peek,nseel_asm_stack_peek_end,1,{0,},NSEEL_PProc_Stack},
  {"stack_exch",nseel_asm_stack_exch,nseel_asm_stack_exch_end,1 | NSEEL_NPARAMS_FLAG_MODSTUFF,{0,},NSEEL_PProc_Stack_PeekTop},
#endif


};

static functionType *fnTableUser;
static int fnTableUser_size;

functionType *nseel_getFunctionFromTable(int idx)
{
  if (idx<0) return 0;
  if (idx>=sizeof(fnTable1)/sizeof(fnTable1[0]))
  {
    idx -= sizeof(fnTable1)/sizeof(fnTable1[0]);
    if (!fnTableUser || idx >= fnTableUser_size) return 0;
    return fnTableUser+idx;
  }
  return fnTable1+idx;
}

int NSEEL_init() // returns 0 on success
{
  NSEEL_quit();
  return 0;
}

void NSEEL_addfunctionex2(const char *name, int nparms, char *code_startaddr, int code_len, void *pproc, void *fptr, void *fptr2)
{
  if (!fnTableUser || !(fnTableUser_size&7))
  {
    fnTableUser=(functionType *)realloc(fnTableUser,(fnTableUser_size+8)*sizeof(functionType));
  }
  if (fnTableUser)
  {
    memset(&fnTableUser[fnTableUser_size],0,sizeof(functionType));
    fnTableUser[fnTableUser_size].nParams = nparms;
    fnTableUser[fnTableUser_size].name = name;
    fnTableUser[fnTableUser_size].afunc = code_startaddr;
    fnTableUser[fnTableUser_size].func_e = code_startaddr + code_len;
    fnTableUser[fnTableUser_size].pProc = (NSEEL_PPPROC) pproc;
    fnTableUser[fnTableUser_size].replptrs[0]=fptr;
    fnTableUser[fnTableUser_size].replptrs[1]=fptr2;
    fnTableUser_size++;
  }
}

void NSEEL_quit()
{
  free(fnTableUser);
  fnTableUser_size=0;
  fnTableUser=0;
}

//---------------------------------------------------------------------------------------------------------------
static void freeBlocks(llBlock **start)
{
  llBlock *s=*start;
  *start=0;
  while (s)
  {
    llBlock *llB = s->next;
    free(s);
    s=llB;
  }
}

//---------------------------------------------------------------------------------------------------------------
static void *__newBlock(llBlock **start, int size, char wantMprotect)
{
#ifdef _WIN32
  DWORD ov;
  UINT_PTR offs,eoffs;
#endif
  llBlock *llb;
  int alloc_size;
  if (*start && (LLB_DSIZE - (*start)->sizeused) >= size)
  {
    void *t=(*start)->block+(*start)->sizeused;
    (*start)->sizeused+=(size+7)&~7;
    return t;
  }

  alloc_size=sizeof(llBlock);
  if ((int)size > LLB_DSIZE) alloc_size += size - LLB_DSIZE;
  llb = (llBlock *)malloc(alloc_size); // grab bigger block if absolutely necessary (heh)
  if (!llb) return NULL;
  
  if (wantMprotect) 
  {
  #ifdef _WIN32
    offs=((UINT_PTR)llb)&~4095;
    eoffs=((UINT_PTR)llb + alloc_size + 4095)&~4095;
    VirtualProtect((LPVOID)offs,eoffs-offs,PAGE_EXECUTE_READWRITE,&ov);
  //  MessageBox(NULL,"vprotecting, yay\n","a",0);
  #elif defined(EEL_USE_MPROTECT)
    {
      static int pagesize = 0;
      if (!pagesize)
      {
        pagesize=sysconf(_SC_PAGESIZE);
        if (!pagesize) pagesize=4096;
      }
      uintptr_t offs,eoffs;
      offs=((uintptr_t)llb)&~(pagesize-1);
      eoffs=((uintptr_t)llb + alloc_size + pagesize-1)&~(pagesize-1);
      mprotect((void*)offs,eoffs-offs,PROT_WRITE|PROT_READ|PROT_EXEC);
    }
  #endif
  }
  llb->sizeused=(size+7)&~7;
  llb->next = *start;  
  *start = llb;
  return llb->block;
}


//---------------------------------------------------------------------------------------------------------------
opcodeRec *nseel_createCompiledValue(compileContext *ctx, EEL_F value)
{
  opcodeRec *r=newOpCode(ctx);
  r->opcodeType = OPCODETYPE_DIRECTVALUE;
  r->parms.dv.directValue = value; 
  r->parms.dv.valuePtr = NULL;
  return r;
}

opcodeRec *nseel_createCompiledValueFromNamespaceName(compileContext *ctx, const char *relName)
{
  int n=strlen(relName);
  opcodeRec *r=(opcodeRec*)__newBlock_align(ctx,sizeof(opcodeRec)+NSEEL_MAX_VARIABLE_NAMELEN,8, ctx->isSharedFunctions ? 0 : -1); 
  if (!r) return 0;
  r->opcodeType=OPCODETYPE_VALUE_FROM_NAMESPACENAME;
  if (n > NSEEL_MAX_VARIABLE_NAMELEN-1) n=NSEEL_MAX_VARIABLE_NAMELEN-1;
  memcpy(r->relname,relName,n);
  r->relname[n]=0;
  return r;
}

opcodeRec *nseel_createCompiledValuePtr(compileContext *ctx, EEL_F *addrValue)
{
  opcodeRec *r=newOpCode(ctx);
  r->opcodeType = OPCODETYPE_VARPTR;
  r->parms.dv.valuePtr=addrValue;
  r->parms.dv.directValue=0.0;
  return r;
}

opcodeRec *nseel_createCompiledValuePtrPtr(compileContext *ctx, EEL_F **addrValue)
{
  opcodeRec *r=newOpCode(ctx);
  r->opcodeType = OPCODETYPE_VARPTRPTR;
  r->parms.dv.valuePtr=(EEL_F *)addrValue;
  r->parms.dv.directValue=0.0;
  return r;
}

opcodeRec *nseel_createCompiledFunctionCallEELThis(compileContext *ctx, _codeHandleFunctionRec *fnp, const char *relName)
{
  int n=strlen(relName);
  opcodeRec *r=(opcodeRec*)__newBlock_align(ctx,sizeof(opcodeRec)+NSEEL_MAX_VARIABLE_NAMELEN,8, ctx->isSharedFunctions ? 0 : -1); 
  if (!r) return 0;
  r->fntype=FUNCTYPE_EELFUNC_THIS;
  r->fn = fnp;
  if (fnp->num_params >= 3) r->opcodeType = OPCODETYPE_FUNC3;
  else if (fnp->num_params==2) r->opcodeType = OPCODETYPE_FUNC2;
  else r->opcodeType = OPCODETYPE_FUNC1;
  if (n > NSEEL_MAX_VARIABLE_NAMELEN-1) n=NSEEL_MAX_VARIABLE_NAMELEN-1;
  memcpy(r->relname,relName,n);
  r->relname[n]=0;

  return r;
}

opcodeRec *nseel_createCompiledFunctionCall(compileContext *ctx, int np, int fntype, void *fn)
{
  opcodeRec *r=newOpCode(ctx);
  if (!r) return 0;
  r->fntype=fntype;
  r->fn = fn;
  if (np >= 3) r->opcodeType = OPCODETYPE_FUNC3;
  else if (np==2) r->opcodeType = OPCODETYPE_FUNC2;
  else r->opcodeType = OPCODETYPE_FUNC1;

  return r;
}

opcodeRec *nseel_setCompiledFunctionCallParameters(opcodeRec *fn, opcodeRec *code1, opcodeRec *code2, opcodeRec *code3)
{
  if (fn)
  {
    opcodeRec *r = fn;
    r->parms.parms[0] = code1;
    r->parms.parms[1] = code2;
    r->parms.parms[2] = code3;
  }
  return fn;
}


opcodeRec *nseel_createSimpleCompiledFunction(compileContext *ctx, int fn, int np, opcodeRec *code1, opcodeRec *code2)
{
  opcodeRec *r=newOpCode(ctx);
  r->opcodeType = np>=2 ? OPCODETYPE_FUNC2:OPCODETYPE_FUNC1;
  r->fntype = fn;
  r->parms.parms[0] = code1;
  r->parms.parms[1] = code2;
  return r;  
}


static int compileOpcodes(compileContext *ctx, opcodeRec *op, unsigned char *bufOut, int bufOut_len, int *computTable, const char *namespacePathToThis);
static unsigned char *compileCodeBlockWithRet(compileContext *ctx, opcodeRec *rec, int *computTableSize, const char *namespacePathToThis);

_codeHandleFunctionRec *eel_createFunctionNamespacedInstance(compileContext *ctx, _codeHandleFunctionRec *fr, const char *nameptr)
{
  int n;
  _codeHandleFunctionRec *subfr = fr->isCommonFunction ? newDataBlock(sizeof(_codeHandleFunctionRec),8) : newTmpBlock(ctx,sizeof(_codeHandleFunctionRec));
  if (!subfr) return 0;
  // fr points to functionname()'s rec, nameptr to blah.functionname()

  *subfr = *fr;
  n = strlen(nameptr);
  if (n > sizeof(subfr->fname)-1) n=sizeof(subfr->fname)-1;
  memcpy(subfr->fname,nameptr,n);
  subfr->fname[n]=0;

  subfr->next = NULL;
  subfr->startptr=0; // make sure this code gets recompiled (with correct member ptrs) for this instance!

  // subfr->derivedCopies already points to the right place
  fr->derivedCopies = subfr; 
  
  return subfr;

}
static void combineNamespaceFields(char *nm, const char *prefix, const char *relname) // nm must be NSEEL_MAX_VARIABLE_NAMELEN+1 bytes
{
  int lfp = prefix ? strlen(prefix) : 0, lrn=strlen(relname);

  while (*relname == '.') // if relname begins with ., then remove a chunk of context from prefix
  {
    relname++;
    while (lfp>0 && prefix[lfp-1] != '.') lfp--;
    if (lfp>0) lfp--;       
  }

  if (lfp > NSEEL_MAX_VARIABLE_NAMELEN-2) lfp=NSEEL_MAX_VARIABLE_NAMELEN-2;
  if (lfp>0) memcpy(nm,prefix,lfp);

  if (lrn > NSEEL_MAX_VARIABLE_NAMELEN - lfp - (lfp>0)) lrn=NSEEL_MAX_VARIABLE_NAMELEN - lfp - (lfp>0);
  if (lrn > 0)
  {
    if (lfp>0) nm[lfp++] = '.';
    memcpy(nm+lfp,relname,lrn);
    lfp+=lrn;
  }
  nm[lfp++]=0;
}

//---------------------------------------------------------------------------------------------------------------
static void *nseel_getBuiltinFunctionAddress(compileContext *ctx, 
      int fntype, void *fn, 
      NSEEL_PPPROC *pProc, void ***replList, 
      void **endP)
{
  switch (fntype)
  {
#define RF(x) *endP = nseel_asm_##x##_end; return (void*)nseel_asm_##x
    case FN_ADD: RF(add);
    case FN_SUB: RF(sub);
    case FN_MULTIPLY: RF(mul);
    case FN_DIVIDE: RF(div);
    case FN_JOIN_STATEMENTS: RF(exec2);
    case FN_AND: RF(and);
    case FN_OR: RF(or);
    case FN_UPLUS: RF(uplus); 
    case FN_UMINUS: RF(uminus);
#undef RF

    case FUNCTYPE_FUNCTIONTYPEREC:
      if (fn)
      {
        functionType *p=(functionType *)fn;
        *replList=p->replptrs;
        *pProc=p->pProc;
        *endP = p->func_e;
        return p->afunc; 
      }
    break;
  }
  
  return 0;
}



static void *nseel_getEELFunctionAddress(compileContext *ctx, 
      opcodeRec *op,
      int *customFuncParmSize, int *customFuncLocalStorageSize,
      EEL_F ***customFuncLocalStorage, int *computTableTop, 
      void **endP, int *isRaw, int wantCodeGenerated,
      const char *namespacePathToThis) // if wantCodeGenerated is false, can return bogus pointers in raw mode
{
  _codeHandleFunctionRec *fn = (_codeHandleFunctionRec*)op->fn;
  switch (op->fntype)
  {
    case FUNCTYPE_EELFUNC_THIS:
      if (fn)
      {
        _codeHandleFunctionRec *fr_base = fn;
        char nm[NSEEL_MAX_VARIABLE_NAMELEN+1];
        combineNamespaceFields(nm,namespacePathToThis,op->relname);

        fn = 0; // if this gets re-set, it will be the new function

        // find resolved function
        if (!fn)
        {
          _codeHandleFunctionRec *fr = fr_base;
          // scan for function
          while (fr && !fn)
          {
            if (!strcasecmp(fr->fname,nm)) fn = fr;
            fr=fr->derivedCopies;
          }
        }

        if (!fn) // generate copy of function
        {
          fn = eel_createFunctionNamespacedInstance(ctx,fr_base,nm);
        }
      }

      // fall through!
    case FUNCTYPE_EELFUNC:
      if (fn)
      {
        const char *fPrefix=NULL;
        char prefix_buf[NSEEL_MAX_VARIABLE_NAMELEN+1];

        _codeHandleFunctionRec *fr = (_codeHandleFunctionRec *) fn;

        if (fr->usesThisPointer)
        {
          char *p=fr->fname;
          while (*p) p++;
          while (p >= fr->fname && *p != '.') p--;
          if (p >= fr->fname)
          {
            int l = p-fr->fname;
            memcpy(prefix_buf,fr->fname,l);
            prefix_buf[l]=0;
            fPrefix = prefix_buf;
          }
          else
          {
            fPrefix = fr->fname; // default prefix is function name if no other context
          }
        }


        if (!fr->startptr && fr->opcodes && fr->startptr_size > 0)
        {
          int sz;
          fr->tmpspace_req=0;

          sz=compileOpcodes(ctx,fr->opcodes,NULL,128*1024*1024,&fr->tmpspace_req,fPrefix);

          if (!wantCodeGenerated)
          {
            // don't compile anything for now, just give stats
            if (computTableTop) *computTableTop += fr->tmpspace_req;
            *customFuncParmSize = fr->num_params;
            *customFuncLocalStorage = fr->localstorage;
            *customFuncLocalStorageSize = fr->localstorage_size;

            if (sz <= NSEEL_MAX_FUNCTION_SIZE_FOR_INLINE)
            {
              *isRaw = 1;
              *endP = ((char *)1) + sz;
              return (char *)1;
            }
            *endP = (void*)nseel_asm_fcall_end;
            return (void*)nseel_asm_fcall;
          }

          if (sz <= NSEEL_MAX_FUNCTION_SIZE_FOR_INLINE)
          {
            void *p=newTmpBlock(ctx,sz);
            fr->tmpspace_req=0;
            if (p)
            {
              sz=compileOpcodes(ctx,fr->opcodes,(unsigned char*)p,fr->startptr_size,&fr->tmpspace_req,fPrefix);
              // recompile function with native context pointers
              if (sz>0)
              {
                fr->startptr_size=sz;
                fr->startptr=p;
              }
            }
          }
          else
          {
            unsigned char *codeCall;
            fr->tmpspace_req=0;
            codeCall=compileCodeBlockWithRet(ctx,fr->opcodes,&fr->tmpspace_req,fPrefix);
            if (codeCall)
            {
              void *f=GLUE_realAddress(nseel_asm_fcall,nseel_asm_fcall_end,&sz);
              fr->startptr = newTmpBlock(ctx,sz);
              if (fr->startptr)
              {
                memcpy(fr->startptr,f,sz);
                EEL_GLUE_set_immediate(fr->startptr,(void *)codeCall);
                fr->startptr_size = sz;
              }
            }
          }
        }
        if (fr->startptr)
        {
          if (computTableTop) *computTableTop += fr->tmpspace_req;
          *customFuncParmSize = fr->num_params;
          *customFuncLocalStorage = fr->localstorage;
          *customFuncLocalStorageSize = fr->localstorage_size;
          *endP = (char*)fr->startptr + fr->startptr_size;
          *isRaw=1;
          return fr->startptr;
        }
      }
    break;
  }
  
  return 0;
}



// returns true if does something (other than calculating and throwing away a value)
static int optimizeOpcodes(compileContext *ctx, opcodeRec *op)
{
  int retv = 0, retv_parm[3]={0,0,0};
  while (op && op->opcodeType == OPCODETYPE_FUNC2 && op->fntype == FN_JOIN_STATEMENTS)
  {
    if (!optimizeOpcodes(ctx,op->parms.parms[0]) || OPCODE_IS_TRIVIAL(op->parms.parms[0]))
    {
      // direct value, can skip ourselves
      memcpy(op,op->parms.parms[1],sizeof(*op));
    }
    else
    {
      retv |= 1;
      op = op->parms.parms[1];
    }
  }
  if (!op || // should never really happen
      OPCODE_IS_TRIVIAL(op) || // should happen often (vars)
      op->opcodeType < 0 || op->opcodeType >= OPCODETYPE_INVALID // should never happen (assert would be appropriate heh)
      ) return retv;

  retv_parm[0] = optimizeOpcodes(ctx,op->parms.parms[0]);
  if (op->opcodeType>=OPCODETYPE_FUNC2) retv_parm[1] = optimizeOpcodes(ctx,op->parms.parms[1]);
  if (op->opcodeType>=OPCODETYPE_FUNC3) retv_parm[2] = optimizeOpcodes(ctx,op->parms.parms[2]);

  if (op->fntype >= 0 && op->fntype < FUNCTYPE_SIMPLEMAX)
  {
    if (op->opcodeType == OPCODETYPE_FUNC1) // within FUNCTYPE_SIMPLE
    {
      if (op->parms.parms[0]->opcodeType == OPCODETYPE_DIRECTVALUE)
      {
        switch (op->fntype)
        {
          case FN_UMINUS:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = - op->parms.parms[0]->parms.dv.directValue;
            op->parms.dv.valuePtr=NULL;
          break;
          case FN_UPLUS:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = op->parms.parms[0]->parms.dv.directValue;
            op->parms.dv.valuePtr=NULL;
          break;
        }
      }
    }
    else if (op->opcodeType == OPCODETYPE_FUNC2)  // within FUNCTYPE_SIMPLE
    {
      int dv0 = op->parms.parms[0]->opcodeType == OPCODETYPE_DIRECTVALUE;
      int dv1 = op->parms.parms[1]->opcodeType == OPCODETYPE_DIRECTVALUE;
      if (dv0 && dv1)
      {
        switch (op->fntype)
        {
          case FN_DIVIDE:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = op->parms.parms[0]->parms.dv.directValue / op->parms.parms[1]->parms.dv.directValue;
            op->parms.dv.valuePtr=NULL;
          break;
          case FN_MULTIPLY:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = op->parms.parms[0]->parms.dv.directValue * op->parms.parms[1]->parms.dv.directValue;
            op->parms.dv.valuePtr=NULL;
          break;
          case FN_ADD:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = op->parms.parms[0]->parms.dv.directValue + op->parms.parms[1]->parms.dv.directValue;
            op->parms.dv.valuePtr=NULL;
          break;
          case FN_SUB:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = op->parms.parms[0]->parms.dv.directValue + op->parms.parms[1]->parms.dv.directValue;
            op->parms.dv.valuePtr=NULL;
          break;
          case FN_AND:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = (double) (((WDL_INT64)op->parms.parms[0]->parms.dv.directValue) & ((WDL_INT64)op->parms.parms[1]->parms.dv.directValue));
            op->parms.dv.valuePtr=NULL;
          break;
          case FN_OR:
            op->opcodeType = OPCODETYPE_DIRECTVALUE;
            op->parms.dv.directValue = (double) (((WDL_INT64)op->parms.parms[0]->parms.dv.directValue) | ((WDL_INT64)op->parms.parms[1]->parms.dv.directValue));
            op->parms.dv.valuePtr=NULL;
          break;
        }
      }
      else if (dv0 || dv1)
      {
        double dvalue = op->parms.parms[!dv0]->parms.dv.directValue;
        switch (op->fntype)
        {
          case FN_SUB:
            if (dv0) 
            {
              if (dvalue == 0.0)
              {
                op->opcodeType = OPCODETYPE_FUNC1;
                op->fntype = FN_UMINUS;
                op->parms.parms[0] = op->parms.parms[1];
              }
              break;
            }
            // fall through, if dv1 we can remove +0.0

          case FN_ADD:
            if (dvalue == 0.0) memcpy(op,op->parms.parms[!!dv0],sizeof(*op));
          break;
          case FN_AND:
            if ((WDL_INT64)dvalue) break;
            dvalue = 0.0; // treat x&0 as x*0, which optimizes to 0
            
            // fall through
          case FN_MULTIPLY:
            if (dvalue == 0.0) // remove multiply by 0.0 (using 0.0 direct value as replacement), unless the nonzero side did something
            {
              if (!retv_parm[!!dv0]) 
              {
                memcpy(op,op->parms.parms[!dv0],sizeof(*op)); // set to 0 if other action wouldn't do anything
              }
              else
              {
                // this is 0.0 * oldexpressionthatmustbeprocessed or oldexpressionthatmustbeprocessed*0.0
                op->fntype = FN_JOIN_STATEMENTS;

                if (dv0) // 0.0*oldexpression, reverse the order so that 0 is returned
                {
                  // set to (oldexpression;0)
                  opcodeRec *tmp = op->parms.parms[1];
                  op->parms.parms[1] = op->parms.parms[0];
                  op->parms.parms[0] = tmp;
                }
              }
            }
            else if (dvalue == 1.0) // remove multiply by 1.0 (using non-1.0 value as replacement)
            {
              memcpy(op,op->parms.parms[!!dv0],sizeof(*op));
            }
          break;
          case FN_DIVIDE:
            if (dv1)
            {
              if (dvalue == 1.0)  // remove divide by 1.0  (using non-1.0 value as replacement)
              {
                memcpy(op,op->parms.parms[!!dv0],sizeof(*op));
              }
              else
              {
                // change to a multiply
                op->fntype = FN_MULTIPLY;
                if (op->parms.parms[1]->parms.dv.directValue == 0.0)
                {
                  optimizeOpcodes(ctx,op); // since we're a multiply, this might reduce us to 0.0 ourselves (or exec2)
                }
                else
                {
                  op->parms.parms[1]->parms.dv.directValue = 1.0/op->parms.parms[1]->parms.dv.directValue;
                  op->parms.parms[1]->parms.dv.valuePtr=NULL;
                }
              }
            }
            else if (dvalue == 0.0)
            {
              if (!retv_parm[!!dv0])
              {
                // if 0/x set to always 0.
                // this is 0.0 / (oldexpression that can be eliminated)
                memcpy(op,op->parms.parms[!dv0],sizeof(*op)); // set to 0 if other action wouldn't do anything
              }
              else
              {
                opcodeRec *tmp;
                // this is 0.0 / oldexpressionthatmustbeprocessed
                op->fntype = FN_JOIN_STATEMENTS;
                tmp = op->parms.parms[1];
                op->parms.parms[1] = op->parms.parms[0];
                op->parms.parms[0] = tmp;
                // set to (oldexpression;0)
              }
            }
          break;
        }
      }
    }
    // FUNCTYPE_SIMPLE
  }   
  else if (op->fntype == FUNCTYPE_FUNCTIONTYPEREC && op->fn)
  {

    /*
    probably worth doing reduction on:
    _divop (constant change to multiply)
    _and
    _or
    _not
    _mod
    _shr
    _shl
    _xor
    abs

    maybe:
    min
    max
    _equal
    _noteq
    _below
    _above
    _beleq
    _aboeq


    also, optimize should (recursively or maybe iteratively?) search transitive functions (mul/div) for more constant reduction possibilities


    */


    functionType  *pfn = (functionType *)op->fn;

    if (pfn->nParams&NSEEL_NPARAMS_FLAG_MODSTUFF) retv|=1;

    if (op->opcodeType==OPCODETYPE_FUNC1) // within FUNCTYPE_FUNCTIONTYPEREC
    {
      if (op->parms.parms[0]->opcodeType == OPCODETYPE_DIRECTVALUE)
      {
        int suc=1;
        EEL_F v = op->parms.parms[0]->parms.dv.directValue;
#define DOF(x) if (!strcmp(pfn->name,#x)) v = x(v); else
        DOF(sin)
        DOF(cos)
        DOF(tan)
        DOF(asin)
        DOF(acos)
        DOF(atan)
        DOF(sqrt)
        DOF(exp)
        DOF(log)
        DOF(log10)
        /*else*/ suc=0;
#undef DOF
        if (suc)
        {
          op->opcodeType = OPCODETYPE_DIRECTVALUE;
          op->parms.dv.directValue = v;
          op->parms.dv.valuePtr=NULL;
        }


      }
    }
    else if (op->opcodeType==OPCODETYPE_FUNC2)  // within FUNCTYPE_FUNCTIONTYPEREC
    {
      if (op->parms.parms[0]->opcodeType == OPCODETYPE_DIRECTVALUE &&
          op->parms.parms[1]->opcodeType == OPCODETYPE_DIRECTVALUE)
      {
        if (pfn->replptrs[0] == &pow || 
            pfn->replptrs[0] == &atan2) 
        {
          op->opcodeType = OPCODETYPE_DIRECTVALUE;
          op->parms.dv.directValue = pfn->replptrs[0]==pow ? 
            pow(op->parms.parms[0]->parms.dv.directValue, op->parms.parms[1]->parms.dv.directValue) :
            atan2(op->parms.parms[0]->parms.dv.directValue, op->parms.parms[1]->parms.dv.directValue);
          op->parms.dv.valuePtr=NULL;
        }
      }
      else if (pfn->replptrs[0] == &pow)
      {
        opcodeRec *first_parm = op->parms.parms[0];
        if (first_parm->opcodeType == op->opcodeType && first_parm->fn == op->fn && first_parm->fntype == op->fntype)
        {            
          // since first_parm is a pow too, we can multiply the exponents.

          // set our base to be the base of the inner pow
          op->parms.parms[0] = first_parm->parms.parms[0];

          // make the old extra pow be a multiply of the exponents
          first_parm->fntype = FN_MULTIPLY;
          first_parm->parms.parms[0] = op->parms.parms[1];

          // put that as the exponent
          op->parms.parms[1] = first_parm;

          retv|=optimizeOpcodes(ctx,op);

        }
      }
    }
    else if (op->opcodeType==OPCODETYPE_FUNC3)  // within FUNCTYPE_FUNCTIONTYPEREC
    {
      if (op->parms.parms[0]->opcodeType == OPCODETYPE_DIRECTVALUE)
      {
        if (!strcmp(pfn->name,"_if"))
        {
          int s = fabs(op->parms.parms[0]->parms.dv.directValue) >= g_closefact;
          memcpy(op,op->parms.parms[s ? 1 : 2],sizeof(opcodeRec));
          retv_parm[s ? 2 : 1]=0; // we're ignoring theo ther code
        }
      }
    }
    // FUNCTYPE_FUNCTIONTYPEREC
  }
  else
  {
    // user func or unknown func time, always does stuff
    // todo: for user tables, support some variant on nParams&NSEEL_NPARAMS_FLAG_MODSTUFF
    retv |= 1;
  }
  return retv||retv_parm[0]||retv_parm[1]||retv_parm[2];
}


static int generateValueToReg(compileContext *ctx, opcodeRec *op, unsigned char *bufOut, int whichReg, const char *functionPrefix)
{
  EEL_F *b=NULL;
  if (op->opcodeType==OPCODETYPE_VALUE_FROM_NAMESPACENAME)
  {
    char nm[NSEEL_MAX_VARIABLE_NAMELEN+1];

    combineNamespaceFields(nm,functionPrefix,op->relname);
    
    b = nseel_int_register_var(ctx,nm);
    if (!b) return -1;
  }
  else
  {
    b=op->parms.dv.valuePtr;
    if (b && op->opcodeType == OPCODETYPE_VARPTRPTR) b = *(EEL_F **)b;

    if (!b)
    {
      b = newDataBlock(sizeof(EEL_F),sizeof(EEL_F));
      if (!b) return -1;

      if (op->opcodeType != OPCODETYPE_VARPTRPTR) op->parms.dv.valuePtr = b;
      #if EEL_F_SIZE == 8
        *b = denormal_filter_double(op->parms.dv.directValue);
      #else
        *b = denormal_filter_float(op->parms.dv.directValue);
      #endif

    }
  }

  GLUE_MOV_PX_DIRECTVALUE_GEN(bufOut,(INT_PTR)b,whichReg);
  return GLUE_MOV_PX_DIRECTVALUE_SIZE;
}


unsigned char *compileCodeBlockWithRet(compileContext *ctx, opcodeRec *rec, int *computTableSize, const char *namespacePathToThis)
{
  unsigned char *p, *newblock2;
  // generate code call
  int funcsz=compileOpcodes(ctx,rec,NULL,1024*1024*128,NULL,namespacePathToThis);
  if (funcsz<0) return NULL;
  
  p = newblock2 = newCodeBlock(funcsz+sizeof(GLUE_RET)+GLUE_FUNC_ENTER_SIZE+GLUE_FUNC_LEAVE_SIZE,32);
  if (!newblock2) return NULL;
  memcpy(p,&GLUE_FUNC_ENTER,GLUE_FUNC_ENTER_SIZE); p += GLUE_FUNC_ENTER_SIZE;       
  p+=compileOpcodes(ctx,rec,p, funcsz, computTableSize,namespacePathToThis);         
  memcpy(p,&GLUE_FUNC_LEAVE,GLUE_FUNC_LEAVE_SIZE); p+=GLUE_FUNC_LEAVE_SIZE;
  memcpy(p,&GLUE_RET,sizeof(GLUE_RET)); p+=sizeof(GLUE_RET);
  
  ctx->l_stats[2]+=funcsz+2;
  return newblock2;
}      



int compileOpcodes(compileContext *ctx, opcodeRec *op, unsigned char *bufOut, int bufOut_len, int *computTableSize, const char *namespacePathToThis)
{
  int rv_offset=0;
  if (!op) return -1;
 
  // special case: statement delimiting means we can process the left side into place, and iteratively do the second parameter without recursing
  // also we don't need to save/restore anything to the stack (which the normal 2 parameter function processing does)
  while (op->opcodeType == OPCODETYPE_FUNC2 && op->fntype == FN_JOIN_STATEMENTS)
  {
    int parm_size = compileOpcodes(ctx,op->parms.parms[0],bufOut,bufOut_len, computTableSize, namespacePathToThis);
    if (parm_size < 0) return -1;
    op = op->parms.parms[1];
    if (!op) return -1;

    if (bufOut) bufOut += parm_size;
    bufOut_len -= parm_size;
    rv_offset += parm_size;
  }


  if (op->fntype == FUNCTYPE_FUNCTIONTYPEREC)
  {
    // special case: while
    functionType *fn_ptr = (functionType *)op->fn;
    if (op->opcodeType == OPCODETYPE_FUNC1 && fn_ptr == fnTable1 + 4)
    {
      unsigned char *newblock2;
      int stubsz;
      void *stubfunc = GLUE_realAddress(nseel_asm_repeatwhile,nseel_asm_repeatwhile_end,&stubsz);
      if (bufOut_len < stubsz) return -1;        
      if (!bufOut) return rv_offset+stubsz;
      
      newblock2=compileCodeBlockWithRet(ctx,op->parms.parms[0],computTableSize,namespacePathToThis);
      if (!newblock2) return -1;
      
      memcpy(bufOut,stubfunc,stubsz);
      bufOut=EEL_GLUE_set_immediate(bufOut,newblock2); 
      EEL_GLUE_set_immediate(bufOut,&g_closefact); 
      
      if (computTableSize) (*computTableSize)++;
      return rv_offset+stubsz;
    }
    
    // special case: BAND/BOR/LOOP
    if (op->opcodeType == OPCODETYPE_FUNC2 && (fn_ptr == fnTable1+1 || fn_ptr == fnTable1+2 || fn_ptr == fnTable1+3))
    {
      void *stub;
      int stubsize;        
      unsigned char *newblock2, *p;
      
      int parm_size = compileOpcodes(ctx,op->parms.parms[0],bufOut,bufOut_len, computTableSize, namespacePathToThis);
      if (parm_size < 0) return -1;
      
      if (fn_ptr == fnTable1+1) stub = GLUE_realAddress(nseel_asm_band,nseel_asm_band_end,&stubsize);
      else if (fn_ptr == fnTable1+3) stub = GLUE_realAddress(nseel_asm_repeat,nseel_asm_repeat_end,&stubsize);
      else stub = GLUE_realAddress(nseel_asm_bor,nseel_asm_bor_end,&stubsize);
      
      if (computTableSize) (*computTableSize) ++;
      
      if (bufOut_len < parm_size + stubsize) return -1;
      
      if (!bufOut)
      {
        return rv_offset + parm_size + stubsize;
      }
      
      newblock2 = compileCodeBlockWithRet(ctx,op->parms.parms[1],computTableSize,namespacePathToThis);
      
      p = bufOut + parm_size;
      memcpy(p, stub, stubsize);
      
      if (fn_ptr!=fnTable1 + 3) p=EEL_GLUE_set_immediate(p,&g_closefact); // for or/and
      p=EEL_GLUE_set_immediate(p,newblock2);
      if (fn_ptr!=fnTable1 + 3) p=EEL_GLUE_set_immediate(p,&g_closefact); // for or/and
  #ifdef __ppc__
      if (fn_ptr!=fnTable1 + 3) // for or/and on ppc we need a one
      {
        p=EEL_GLUE_set_immediate(p,&eel_one);
      }
  #endif
      return rv_offset + parm_size + stubsize;
    }  
    
    if (op->opcodeType == OPCODETYPE_FUNC3 && fn_ptr == fnTable1 + 0) // special case: IF
    {
      void *stub;
      unsigned char *newblock2,*newblock3,*ptr;
      int stubsize;
      int parm_size = compileOpcodes(ctx,op->parms.parms[0],bufOut,bufOut_len, computTableSize, namespacePathToThis);
      if (parm_size < 0) return -1;
      
      stub = GLUE_realAddress(nseel_asm_if,nseel_asm_if_end,&stubsize);

      if (computTableSize) (*computTableSize) ++;
      
      if (bufOut_len < parm_size + stubsize) return -1;
      
      if (!bufOut)
      {
        return rv_offset + parm_size + stubsize;
      }
      
      
      newblock2 = compileCodeBlockWithRet(ctx,op->parms.parms[1],computTableSize,namespacePathToThis);
      newblock3 = compileCodeBlockWithRet(ctx,op->parms.parms[2],computTableSize,namespacePathToThis);
      if (!newblock2 || !newblock3) return -1;
      
      ptr = bufOut + parm_size;
      memcpy(ptr, stub, stubsize);
           
      ptr=EEL_GLUE_set_immediate(ptr,&g_closefact);
      ptr=EEL_GLUE_set_immediate(ptr,newblock2);
      EEL_GLUE_set_immediate(ptr,newblock3);
      
      return rv_offset + parm_size + stubsize;
    }
    
  }
    
  
  
  
  switch (op->opcodeType)
  {
    case OPCODETYPE_VALUE_FROM_NAMESPACENAME:
    case OPCODETYPE_DIRECTVALUE:
    case OPCODETYPE_VARPTR:
    case OPCODETYPE_VARPTRPTR:
      if (bufOut_len < GLUE_MOV_PX_DIRECTVALUE_SIZE) 
      {
        return -1;
      }
      if (bufOut) 
      {
        if (generateValueToReg(ctx,op,bufOut,0,namespacePathToThis)<0) return -1;
      }
    return rv_offset + GLUE_MOV_PX_DIRECTVALUE_SIZE;
    case OPCODETYPE_FUNC1:
    case OPCODETYPE_FUNC2:
    case OPCODETYPE_FUNC3:
      
      {
        const int n_params = 1 + op->opcodeType - OPCODETYPE_FUNC1;
       
        int func_size=0, parm_size=0;
        int pn;
        int last_nt_parm=-1;
        void *func_e=NULL;
        
        if (computTableSize) (*computTableSize) ++;
        
        if (op->fntype == FUNCTYPE_EELFUNC_THIS || op->fntype == FUNCTYPE_EELFUNC)
        {
          int cfp_numparams=-1;
          int cfp_statesize=0;
          EEL_F **cfp_ptrs=NULL;
          int func_raw=0;
          int do_parms;

          void *func = nseel_getEELFunctionAddress(ctx, op,
                                              &cfp_numparams,&cfp_statesize,&cfp_ptrs, 
                                              computTableSize, 
                                              &func_e, &func_raw,                                              
                                              !!bufOut,namespacePathToThis);

          if (func_raw) func_size = (char*)func_e  - (char*)func;
          else if (func) func = GLUE_realAddress(func,func_e,&func_size);
          
          if (!func) return -1;

          // user defined function
          do_parms = cfp_numparams>0 && cfp_ptrs && cfp_statesize>0;

          // if function local/parameter state is zero, we need to allocate storage for it
          if (cfp_statesize>0 && cfp_ptrs && !cfp_ptrs[0])
          {
            EEL_F *pstate = newDataBlock(sizeof(EEL_F)*cfp_statesize,8);
            if (!pstate) return -1;

            for (pn=0;pn<cfp_statesize;pn++)
            {
              pstate[pn]=0;
              cfp_ptrs[pn] = pstate + pn;
            }
          }

          // first process parameters that are non-trivial
          for (pn=0; pn < n_params; pn++)
          { 
            int lsz;
            if (OPCODE_IS_TRIVIAL(op->parms.parms[pn])) continue; // skip and process after

            if (last_nt_parm >= 0 && do_parms)
            {
              if (bufOut_len < parm_size + (int)sizeof(GLUE_PUSH_P1PTR_AS_VALUE)) return -1;
              
              // push
              if (bufOut) memcpy(bufOut + parm_size,&GLUE_PUSH_P1PTR_AS_VALUE,sizeof(GLUE_PUSH_P1PTR_AS_VALUE));
              parm_size+=sizeof(GLUE_PUSH_P1PTR_AS_VALUE);
            }

            lsz = compileOpcodes(ctx,op->parms.parms[pn],bufOut ? bufOut + parm_size : NULL,bufOut_len - parm_size, computTableSize, namespacePathToThis);
            if (lsz<0) return -1;
            parm_size += lsz;

            last_nt_parm = pn;
          }
          // pop non-trivial results into place
          if (last_nt_parm >=0 && do_parms)
          {
            while (--pn >= 0)
            { 
              if (OPCODE_IS_TRIVIAL(op->parms.parms[pn])) continue; // skip and process after
              if (pn == last_nt_parm)
              {
                // copy direct p1ptr to mem
                const int cpsize = GLUE_COPY_VALUE_AT_P1_TO_PTR(NULL,NULL);
                if (bufOut_len < parm_size + cpsize) return -1;

                if (bufOut) GLUE_COPY_VALUE_AT_P1_TO_PTR((unsigned char *)bufOut + parm_size,cfp_ptrs[pn]);
                parm_size += cpsize;
              }
              else
              {
                const int popsize =  GLUE_POP_VALUE_TO_ADDR(NULL,NULL);
                if (bufOut_len < parm_size + popsize) return -1;

                if (bufOut) GLUE_POP_VALUE_TO_ADDR((unsigned char *)bufOut + parm_size,cfp_ptrs[pn]);
                parm_size+=popsize;

              }
            }
          }

          // finally, set any trivial parameters
          if (do_parms)
          {
            const int cpsize = GLUE_MOV_PX_DIRECTVALUE_SIZE + GLUE_COPY_VALUE_AT_P1_TO_PTR(NULL,NULL);
            for (pn=0; pn < n_params; pn++)
            { 
              if (!OPCODE_IS_TRIVIAL(op->parms.parms[pn])) continue; // set trivial values, we already set nontrivials

              if (bufOut_len < parm_size + cpsize) return -1;

              if (bufOut) 
              {
                if (generateValueToReg(ctx,op->parms.parms[pn],bufOut + parm_size,0,namespacePathToThis)<0) return -1;
                GLUE_COPY_VALUE_AT_P1_TO_PTR(bufOut + parm_size + GLUE_MOV_PX_DIRECTVALUE_SIZE,cfp_ptrs[pn]);
              }
              parm_size += cpsize;

            }
          }


 
          if (bufOut_len < parm_size + func_size) return -1;
          
          if (bufOut) memcpy(bufOut + parm_size, func, func_size);
          
          return rv_offset + parm_size + func_size;
          // end of EEL function generation
        }
        else
        {
          // builtin function generation
          NSEEL_PPPROC preProc=0;
          void **repl=NULL;
          void *func = nseel_getBuiltinFunctionAddress(ctx, op->fntype, op->fn, &preProc,&repl,&func_e);

          if (!func) return -1;

          if (op->parms.parms[0]->opcodeType == OPCODETYPE_DIRECTVALUE)
          {
#ifdef EEL_STACK_SUPPORT
            if (func == nseel_asm_stack_pop)
            {
              func = GLUE_realAddress(nseel_asm_stack_pop_fast,nseel_asm_stack_pop_fast_end,&func_size);
              if (!func || bufOut_len < func_size) return -1;

              if (bufOut) 
              {
                memcpy(bufOut,func,func_size);
                NSEEL_PProc_Stack(bufOut,func_size,ctx);
              }
              return rv_offset + func_size;            
            }
            else if (func == nseel_asm_stack_peek)
            {
              int f = (int) op->parms.parms[0]->parms.dv.directValue;
              if (!f)
              {
                func = GLUE_realAddress(nseel_asm_stack_peek_top,nseel_asm_stack_peek_top_end,&func_size);
                if (!func || bufOut_len < func_size) return -1;

                if (bufOut) 
                {
                  memcpy(bufOut,func,func_size);
                  NSEEL_PProc_Stack_PeekTop(bufOut,func_size,ctx);
                }
                return rv_offset + func_size;
              }
              else
              {
                func = GLUE_realAddress(nseel_asm_stack_peek_int,nseel_asm_stack_peek_int_end,&func_size);
                if (!func || bufOut_len < func_size) return -1;

                if (bufOut)
                {
                  memcpy(bufOut,func,func_size);
                  NSEEL_PProc_Stack_PeekInt(bufOut,func_size,ctx,f*sizeof(EEL_F));
                }
                return rv_offset + func_size;
              }
            }
#endif // EEL_STACK_SUPPORT
          }
          else if (func == nseel_asm_assign &&
              (op->parms.parms[1]->opcodeType == OPCODETYPE_DIRECTVALUE
#if 0
               ||op->parms.parms[1]->opcodeType == OPCODETYPE_VARPTR
               ||op->parms.parms[1]->opcodeType == OPCODETYPE_VARPTRPTR
            // this might be a bad idea, since if someone does x*=0.01; a lot it will probably denormal, and this could let y=x; have it propagate
            // or maybe we need to fix the mulop/divop/etc modes?
#endif
               ))
          {
            // assigning a value (from a variable or other non-computer), can use a fast assign (no denormal/result checking)
             #ifndef __ppc__
               func = nseel_asm_assign_fast;
               func_e = nseel_asm_assign_fast_end;
             #endif
          }

          // end of built-in function specific special casing
          func = GLUE_realAddress(func,func_e,&func_size);
          if (!func) return -1;

          // first pass, calculate any non-trivial parameters
          for (pn=0; pn < n_params; pn++)
          { 
            int lsz=0; 
            if (OPCODE_IS_TRIVIAL(op->parms.parms[pn])) continue; // skip and process after
            if (last_nt_parm>=0)
            {
              // push last result
              if (bufOut_len < parm_size + (int)sizeof(GLUE_PUSH_P1)) return -1;
              if (bufOut) memcpy(bufOut + parm_size, &GLUE_PUSH_P1, sizeof(GLUE_PUSH_P1));
              parm_size += sizeof(GLUE_PUSH_P1);
            }
          
            lsz = compileOpcodes(ctx,op->parms.parms[pn],bufOut ? bufOut + parm_size : NULL,bufOut_len - parm_size, computTableSize, namespacePathToThis);
            if (lsz<0) return -1;
            parm_size += lsz;            
            last_nt_parm = pn;
          }

          // pop/copy non-trivial values to their correct places
          for (pn=0; pn < n_params; pn++)
          { 
            if (OPCODE_IS_TRIVIAL(op->parms.parms[pn])) continue; 

            if (pn == last_nt_parm)
            {
              if (pn != n_params-1)
              {
                // generate mov p1->pX
                if (bufOut_len < parm_size + GLUE_SET_PX_FROM_P1_SIZE) return -1;
                if (bufOut) GLUE_SET_PX_FROM_P1(bufOut + parm_size,n_params-1 - pn);
                parm_size += GLUE_SET_PX_FROM_P1_SIZE;
              }
            }
            else
            {
              if (bufOut_len < parm_size + GLUE_POP_PX_SIZE) return -1;
              if (bufOut) GLUE_POP_PX(bufOut + parm_size,n_params-1 - pn);
              parm_size += GLUE_POP_PX_SIZE;
            }           
          }

          // finally, set trivial pointers
          for (pn=0; pn < n_params; pn++)
          { 
            if (!OPCODE_IS_TRIVIAL(op->parms.parms[pn])) continue; 
            if (bufOut_len < parm_size + GLUE_MOV_PX_DIRECTVALUE_SIZE) return -1;
            if (bufOut) 
            {
              if (generateValueToReg(ctx,op->parms.parms[pn],bufOut + parm_size,n_params-1 - pn,namespacePathToThis)<0) return -1;
            }
            parm_size += GLUE_MOV_PX_DIRECTVALUE_SIZE;
          }
          
                             
          if (bufOut_len < parm_size + func_size) return -1;
          
          if (bufOut)
          {
            unsigned char *p=bufOut + parm_size;
            memcpy(p, func, func_size);
            if (preProc) preProc(p,func_size,ctx);
            if (repl)
            {
              if (repl[0]) p=EEL_GLUE_set_immediate(p,repl[0]);
              if (repl[1]) p=EEL_GLUE_set_immediate(p,repl[1]);
              if (repl[2]) p=EEL_GLUE_set_immediate(p,repl[2]);
              if (repl[3]) p=EEL_GLUE_set_immediate(p,repl[3]);
            }
          }
          return rv_offset + parm_size + func_size;
          // end of builtin function generation
        }        
      }
    return -1;
  }
  return -1; // invalid opcode
}

static char *preprocessCode(compileContext *ctx, char *expression)
{
  char *expression_start=expression;
  int len=0;
  int alloc_len=strlen(expression)+1+64;
  char *buf=(char *)malloc(alloc_len);
  int semicnt=0;
  // we need to call onCompileNewLine for each new line we get
 
  //onCompileNewLine(ctx, 

  while (*expression)
  {
    if (len > alloc_len-64)
    {
      alloc_len = len+128;
      buf=(char*)realloc(buf,alloc_len);
    }

    if (expression[0] == '/')
    {
      if (expression[1] == '/')
      {
        expression+=2;
        while (expression[0] && expression[0] != '\n') expression++;
	      continue;
      }
      else if (expression[1] == '*')
      {
        expression+=2;
        while (expression[0] && (expression[0] != '*' || expression[1] != '/')) 
	      {
		      if (expression[0] == '\n') onCompileNewLine(ctx,expression+1-expression_start,len);
		      expression++;
	      }
        if (expression[0]) expression+=2; // at this point we KNOW expression[0]=* and expression[1]=/
	      continue;
      }
    }
    
    if (expression[0] == '(' && expression[1]==')')
    {
      expression+=2;
      memcpy(buf+len,"(0)",3);
      len+=3;
      ctx->l_stats[0]+=3;
      continue;
    }
    if (expression[0] == '$')
    {
      if (toupper(expression[1]) == 'X'||expression[1] == '~')
      {
        char isBits = expression[1] == '~';
        char *p=expression+2;
        unsigned int v=strtoul(expression+2,&p,isBits ? 10 : 16);
        char tmp[256];
        expression=p;

        if (isBits)
        {
          if (v<0) v=0;
          if (v>53) v=53;
          sprintf(tmp,"%.1f",(double) ((((WDL_INT64)1) << v) - 1));
        }
        else
        {
          sprintf(tmp,"%u",v);
        }
        memcpy(buf+len,tmp,strlen(tmp));
        len+=strlen(tmp);
        ctx->l_stats[0]+=strlen(tmp);
        continue;

      }
      if (expression[1]=='\'' && expression[2] && expression[3]=='\'')
      {
        char tmp[64];
        sprintf(tmp,"%u",((unsigned char *)expression)[2]);
        expression+=4;

        memcpy(buf+len,tmp,strlen(tmp));
        len+=strlen(tmp);
        ctx->l_stats[0]+=strlen(tmp);
        continue;
      }
      if (toupper(expression[1]) == 'P' && toupper(expression[2]) == 'I')
      {
        static char *str="3.141592653589793";
        expression+=3;
        memcpy(buf+len,str,17);
        len+=17; //strlen(str);
        ctx->l_stats[0]+=17;
	      continue;
      }
      if (toupper(expression[1]) == 'E')
      {
        static char *str="2.71828183";
        expression+=2;
        memcpy(buf+len,str,10);
        len+=10; //strlen(str);
        ctx->l_stats[0]+=10;
  	    continue;
      }
      if (toupper(expression[1]) == 'P' && toupper(expression[2]) == 'H' && toupper(expression[3]) == 'I')
      {
        static char *str="1.61803399";
        expression+=4;
        memcpy(buf+len,str,10);
        len+=10; //strlen(str);
        ctx->l_stats[0]+=10;
	      continue;
      }
      
    }

    {
      char c=*expression++;

      if (c == '\n') onCompileNewLine(ctx,expression-expression_start,len);
      if (isspace(c)) c=' ';

      if (c == '(') semicnt++;
      else if (c == ')') { semicnt--; if (semicnt < 0) semicnt=0; }
      else if (c == ';' && semicnt > 0)
      {
        // convert ; to % if next nonwhitespace char is alnum, otherwise convert to space
        int p=0;
        int nc;
		int commentstate=0;
        	while ((nc=expression[p]))
		{
			if (!commentstate && nc == '/')
			{
				if (expression[p+1] == '/') commentstate=1;
				else if (expression[p+1] == '*') commentstate=2;
			}

			if (commentstate == 1 && nc == '\n') commentstate=0;
			else if (commentstate == 2 && nc == '*' && expression[p+1]=='/')
			{
				p++; // skip *
				commentstate=0;
			}
			else if (!commentstate && !isspace(nc)) break;

			p++;
		}
		// fucko, we should look for even more chars, me thinks
        if (nc && (isalnum(nc) 
#if 1
				|| nc == '(' || nc == '_' || nc == '!' || nc == '$' 
#endif
				)) c='%';
        else c = ' '; // stray ;
      }
#if 0
      else if (semicnt > 0 && c == ',')
      {
        int p=0;
        int nc;
        while ((nc=expression[p]) && isspace(nc)) p++;
		if (nc == ',' || nc == ')') 
		{
			expression += p+1;
			buf[len++]=',';
			buf[len++]='0';
			c=nc; // append this char
		}
      }
#endif
	  // list of operators

	  else if (!isspace(c) && !isalnum(c)) // check to see if this operator is ours
	  {

			static char *symbollists[]=
			{
				"", // or any control char that is not parenthed
				":(,;?%", 
				",):?;", // or || or &&
				",);", // jf> removed :? from this, for =
				",);",
        "",  // only scans for a negative ] level

			};


			static struct 
			{
			  char op[2];
			  char lscan,rscan;
			  char *func;
			} preprocSymbols[] = 
			{
				{{'+','='}, 0, 3, "_addop" },
				{{'-','='}, 0, 3, "_subop" },
				{{'%','='}, 0, 3, "_modop" },
				{{'|','='}, 0, 3, "_orop" },
				{{'&','='}, 0, 3, "_andop"},
				{{'~','='}, 0, 3, "_xorop" },

				{{'/','='}, 0, 3, "_divop"},
				{{'*','='}, 0, 3, "_mulop"},
				{{'^','='}, 0, 3, "_powop"},

				{{'=','='}, 1, 2, "_equal" },
				{{'<','='}, 1, 2, "_beleq" },
				{{'>','='}, 1, 2, "_aboeq" },
				{{'<','<'}, 0, 0, "_shl" },
				{{'>','>'}, 0, 0, "_shr" },
				{{'<',0  }, 1, 2, "_below" },
				{{'>',0  }, 1, 2, "_above" },
				{{'!','='}, 1, 2, "_noteq" },
				{{'|','|'}, 1, 2, "_or" },
				{{'&','&'}, 1, 2, "_and" },
				{{'=',0  }, 0, 3, "_set" },
				{{'~',0},   0, 0, "_xor" },
				{{'%',0},   0, 0, "_mod" },
				{{'^',0},   0, 0, "pow" },


        {{'[',0  }, 0, 5, },
				{{'!',0  },-1, 0, }, // this should also ignore any leading +-
				{{'?',0  }, 1, 4, },

			};


			int n;
			int ns=sizeof(preprocSymbols)/sizeof(preprocSymbols[0]);
			for (n = 0; n < ns; n++)
			{
				if (c == preprocSymbols[n].op[0] && (!preprocSymbols[n].op[1] || expression[0] == preprocSymbols[n].op[1])) 
				{
					break;
				}
			}
			if (n < ns)
			{

				int lscan=preprocSymbols[n].lscan;
				int rscan=preprocSymbols[n].rscan;

	      // parse left side of =, scanning back for an unparenthed nonwhitespace nonalphanumeric nonparenth?
	      // so megabuf(x+y)= would be fine, x=, but +x= would do +set(x,)
       	char *l_ptr=0;
				char *r_ptr=0;
	      if (lscan >= 0)
				{
					char *scan=symbollists[lscan];
	       	int l_semicnt=0;
					l_ptr=buf + len - 1;
					while (l_ptr >= buf)
					{
						if (*l_ptr == ')') l_semicnt++;
						else if (*l_ptr == '(')
						{
							l_semicnt--;
							if (l_semicnt < 0) break;
						}
						else if (!l_semicnt) 
						{
							if (!*scan)
							{
								if (!isspace(*l_ptr) && !isalnum(*l_ptr) && *l_ptr != '_' && *l_ptr != '.') break;
							}
							else
							{
								char *sc=scan;
								if (lscan == 2 && ( // not currently used, even
									(l_ptr[0]=='|' && l_ptr[1] == '|')||
									(l_ptr[0]=='&' && l_ptr[1] == '&')
									)
								   ) break;
								while (*sc && *l_ptr != *sc) sc++;
								if (*sc) break;
							}
						}
						l_ptr--;
					}
					buf[len]=0;

					l_ptr++;

					len = l_ptr - buf;

					l_ptr = strdup(l_ptr); // doesn't need to be preprocessed since it just was
        }
				if (preprocSymbols[n].op[1]) expression++;

				r_ptr=expression;
				{ 
					// scan forward to an uncommented,  unparenthed semicolon, comma, or )
					int r_semicnt=0;
					int r_qcnt=0;
					char *scan=symbollists[rscan];
					int commentstate=0;
					int hashadch=0;
          int bracketcnt=0;
					while (*r_ptr)
					{
						if (!commentstate && *r_ptr == '/')
						{
							if (r_ptr[1] == '/') commentstate=1;
							else if (r_ptr[1] == '*') commentstate=2;
						}
						if (commentstate == 1 && *r_ptr == '\n') commentstate=0;
						else if (commentstate == 2 && *r_ptr == '*' && r_ptr[1]=='/')
						{
							r_ptr++; // skip *
							commentstate=0;
						}
						else if (!commentstate)
						{
							if (*r_ptr == '(') {hashadch=1; r_semicnt++; }
							else if (*r_ptr == ')') 
							{
								r_semicnt--;
								if (r_semicnt < 0) break;
							}
							else if (!r_semicnt)
							{
								char *sc=scan;
								if (*r_ptr == ';' || *r_ptr == ',') break;
							
								if (!rscan)
								{
									if (*r_ptr == ':') break;
									if (!isspace(*r_ptr) && !isalnum(*r_ptr) && *r_ptr != '_' && *r_ptr != '.' && hashadch) break;
									if (isalnum(*r_ptr) || *r_ptr == '_')hashadch=1;
								}								
								else if (rscan == 2 &&
									((r_ptr[0]=='|' && r_ptr[1] == '|')||
									(r_ptr[0]=='&' && r_ptr[1] == '&')
									)
								   ) break;

								else if (rscan == 3 || rscan == 4)
								{
									if (*r_ptr == ':') r_qcnt--;
									else if (*r_ptr == '?') r_qcnt++;

									if (r_qcnt < 3-rscan) break;
								}
                else if (rscan == 5)
                {
                  if (*r_ptr == '[') bracketcnt++;
                  else if (*r_ptr == ']') bracketcnt--;
                  if (bracketcnt < 0) break;
                }

								while (*sc && *r_ptr != *sc) sc++;
								if (*sc) break;
							}
						}
						r_ptr++;
					}
					// expression -> r_ptr is our string (not including r_ptr)

					{
						char *orp=r_ptr;

						char rps=*orp;
						*orp=0; // temporarily terminate

						r_ptr=preprocessCode(ctx,expression);
						expression=orp;

						*orp = rps; // fix termination(restore string)
					}

				}

				if (r_ptr)
				{
					int thisl = strlen(l_ptr?l_ptr:"") + strlen(r_ptr) + 32;

	    			if (len+thisl > alloc_len-64)
    				{
      					alloc_len = len+thisl+128;
      					buf=(char*)realloc(buf,alloc_len);
    				}


          if (n == ns-3)
          {
            char *lp = l_ptr;
            char *rp = r_ptr;
      	    while (lp && *lp && isspace(*lp)) lp++;
      	    while (rp && *rp && isspace(*rp)) rp++;
            if (lp && !strncasecmp(lp,"gmem",4) && (!lp[4] || isspace(lp[4])))
            {
              len+=sprintf(buf+len,"_gmem(%s",r_ptr && *r_ptr ? r_ptr : "0");
              ctx->l_stats[0]+=strlen(l_ptr)+4;
            }
            else if (rp && *rp && strcmp(rp,"0"))
            {
  	          len+=sprintf(buf+len,"_mem((%s)+(%s)",lp,rp);
              ctx->l_stats[0]+=strlen(lp)+strlen(rp)+8;
            }
            else 
            {
	            len+=sprintf(buf+len,"_mem(%s",lp);
              ctx->l_stats[0]+=strlen(lp)+4;
            }

            // skip the ]
            if (*expression == ']') expression++;

          }
					else if (n == ns-2)
					{
						len+=sprintf(buf+len,"_not(%s",r_ptr);

						ctx->l_stats[0]+=4;
					}
					else if (n == ns-1)// if (l_ptr,r_ptr1,r_ptr2)
					{
						char *rptr2=r_ptr;
						char *tmp=r_ptr;
						int parcnt=0;
						int qcnt=1;
						while (*rptr2)
						{
							if (*rptr2 == '?') qcnt++;
							else if (*rptr2 == ':') qcnt--;
							else if (*rptr2 == '(') parcnt++;
							else if (*rptr2 == ')') parcnt--;
							if (parcnt < 0) break;
							if (!parcnt && !qcnt && *rptr2 == ':') break;
							rptr2++;
						}
						if (*rptr2) *rptr2++=0;
						while (isspace(*rptr2)) rptr2++;

						while (isspace(*tmp)) tmp++;

						len+=sprintf(buf+len,"_if(%s,%s,%s",l_ptr,*tmp?tmp:"0",*rptr2?rptr2:"0");
						ctx->l_stats[0]+=6;
					}
					else
					{
						len+=sprintf(buf+len,"%s(%s,%s",preprocSymbols[n].func,l_ptr?l_ptr:"",r_ptr);
						ctx->l_stats[0]+=strlen(preprocSymbols[n].func)+2;
					}

				}

				free(r_ptr);
				free(l_ptr);


				c = ')'; // close parenth below
		  }
	  }

//      if (c != ' ' || (len && buf[len-1] != ' ')) // don't bother adding multiple spaces
      {
      	buf[len++]=c;
      	if (c != ' ') ctx->l_stats[0]++;
      }
    }
  }
  buf[len]=0;

  return buf;
}

#ifdef PPROC_TEST

int main(int argc, char* argv[])
{
	compileContext ctx={0};
	char *p=preprocessCode(&ctx,argv[1]);
	if (p)printf("%s\n",p);
	free(p);
	return 0;
}

#endif

#if 0
static void movestringover(char *str, int amount)
{
  char tmp[1024+8];

  int l=(int)strlen(str);
  l=min(1024-amount-1,l);

  memcpy(tmp,str,l+1);

  while (l >= 0 && tmp[l]!='\n') l--;
  l++;

  tmp[l]=0;//ensure we null terminate

  memcpy(str+amount,tmp,l+1);
}
#endif

//------------------------------------------------------------------------------
NSEEL_CODEHANDLE NSEEL_code_compile(NSEEL_VMCTX _ctx, char *_expression, int lineoffs)
{
  return NSEEL_code_compile_ex(_ctx,_expression,lineoffs,0);
}

typedef struct topLevelCodeSegmentRec {
  struct topLevelCodeSegmentRec *_next;
  void *code;
  int codesz;
} topLevelCodeSegmentRec;

NSEEL_CODEHANDLE NSEEL_code_compile_ex(NSEEL_VMCTX _ctx, char *_expression, int lineoffs, int compile_flags)
{
  compileContext *ctx = (compileContext *)_ctx;
  char *expression,*expression_start;
  codeHandleType *handle;
  topLevelCodeSegmentRec *startpts_tail=NULL;
  topLevelCodeSegmentRec *startpts=NULL;
  int curtabptr_sz=0;
  void *curtabptr=NULL;
  int had_err=0;

  if (!ctx) return 0;

  if (compile_flags & NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS_RESET)
  {
    ctx->functions_common=NULL; // reset common function list
  }
  else
  {
    // reset common compiled function code, forcing a recompile if shared
    _codeHandleFunctionRec *a = ctx->functions_common;
    while (a)
    {
      _codeHandleFunctionRec *b = a->derivedCopies;

      if (a->localstorage) 
      {
        // force local storage actual values to be reallocated if used again
        memset(a->localstorage,0,sizeof(EEL_F *) * a->localstorage_size);
      }

      a->startptr = NULL; // force this copy to be recompiled

      while (b)
      {
        b->startptr = NULL; // force derived copies to get recompiled
        // no need to reset b->localstorage, since it points to a->localstorage
        b=b->derivedCopies;
      }

      a=a->next;
    }
  }
  
  ctx->last_error_string[0]=0;

  if (!_expression || !*_expression) return 0;

  ctx->isSharedFunctions = !!(compile_flags & NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);
  ctx->functions_local = NULL;

  freeBlocks(&ctx->tmpblocks_head);  // free blocks
  freeBlocks(&ctx->blocks_head);  // free blocks
  freeBlocks(&ctx->blocks_head_data);  // free blocks
  memset(ctx->l_stats,0,sizeof(ctx->l_stats));
  free(ctx->compileLineRecs); ctx->compileLineRecs=0; ctx->compileLineRecs_size=0; ctx->compileLineRecs_alloc=0;

  handle = (codeHandleType*)newDataBlock(sizeof(codeHandleType),8);

  if (!handle) 
  {
    return 0;
  }
  
  memset(handle,0,sizeof(codeHandleType));

  ctx->tmpCodeHandle = handle;
  expression_start=expression=preprocessCode(ctx,_expression);

  while (*expression)
  {
    int computTableTop = 0;
    int startptr_size=0;
    void *startptr=NULL;
    opcodeRec *start_opcode=NULL;
    char *expr;
    int function_numparms=0;
    char is_fname[NSEEL_MAX_VARIABLE_NAMELEN+1];
    is_fname[0]=0;

    memset(ctx->function_localTable_Size,0,sizeof(ctx->function_localTable_Size));
    memset(ctx->function_localTable_Names,0,sizeof(ctx->function_localTable_Names));
    ctx->function_localTable_ValuePtrs=0;

    ctx->function_usesThisPointer=0;
    
    ctx->colCount=0;

    
    // single out segment
    while (*expression == ';' || isspace(*expression)) expression++;
    if (!*expression) break;
    expr=expression;

    while (*expression && *expression != ';') expression++;
    if (*expression) *expression++ = 0;

    // parse   

    if (!strncasecmp(expr,"function",8) && isspace(expr[8]))
    {
      char *p = expr+8;
      while (isspace(p[0])) p++;
      if (isalpha(p[0]) || p[0] == '_') 
      {
        int had_parms_locals=0;
        char *sp=p;
        int l;
        while (isalnum(p[0]) || p[0] == '_') p++;
        l=min(p-sp, sizeof(is_fname)-1);
        memcpy(is_fname, sp, l);
        is_fname[l]=0;

        expr = p;

        while (*expr)
        {
          p=expr;
          while (isspace(*p)) p++;
        
          if (p[0] == '(' || 
              !strncasecmp(p,"local(",6)  ||
              !strncasecmp(p,"static(",7) ||
              !strncasecmp(p,"instance(",9))
          {
            int maxcnt=0,state=0;
            int is_parms = 0;
            int localTableContext = 0;

            if (had_parms_locals && p[0] == '(') break; 
            had_parms_locals=1;

            if (p[0] == '(') 
            {
              is_parms = 1;
            }
            else if (!strncasecmp(p,"instance(",9))
            {
              localTableContext = 1; //adding to implied this table
            }

            // skip past parens
            while (*p && *p != '(') p++;
            if (*p) p++;

            sp=p;
            while (*p && *p != ')') 
            {
              if (isspace(*p) || *p == ',')
              {
                if (state) maxcnt++;
                state=0;
              }
              else state=1;
              p++;
            }
            if (state) maxcnt++;
            if (*p)
            {
              expr=p+1;
          
              if (maxcnt > 0)
              {
                char *ot = ctx->function_localTable_Names[localTableContext];
                int osz = ctx->function_localTable_Size[localTableContext];

                maxcnt += osz;

                if (ctx->isSharedFunctions)
                {
                  ctx->function_localTable_Names[localTableContext] = newDataBlock(NSEEL_MAX_VARIABLE_NAMELEN * maxcnt,1);
                }
                else 
                {
                  ctx->function_localTable_Names[localTableContext] = newTmpBlock(ctx,NSEEL_MAX_VARIABLE_NAMELEN * maxcnt);
                }

                if (ctx->function_localTable_Names[localTableContext])
                {
                  int i=osz;
                  if (osz && ot) memcpy(ctx->function_localTable_Names[localTableContext],ot,NSEEL_MAX_VARIABLE_NAMELEN * osz);
                  p=sp;
                  while (p < expr-1 && i < maxcnt)
                  {
                    while (p < expr && (isspace(*p) || *p == ',')) p++;
                    sp=p;
                    while (p < expr-1 && (!isspace(*p) && *p != ',')) p++;
                    
                    if (isalpha(*sp) || *sp == '_')
                    {
                      int use_i = i++;

                      memset(ctx->function_localTable_Names[localTableContext] + use_i*NSEEL_MAX_VARIABLE_NAMELEN, 0, NSEEL_MAX_VARIABLE_NAMELEN);
                      strncpy(ctx->function_localTable_Names[localTableContext] + use_i * NSEEL_MAX_VARIABLE_NAMELEN, sp, min(NSEEL_MAX_VARIABLE_NAMELEN,p-sp));               
                    }
                  }


                  ctx->function_localTable_Size[localTableContext]=i;

                  if (is_parms) function_numparms = i;
                }
              }
            }         
          }
          else break;
        }
      }
    }
    if (ctx->function_localTable_Size>0)
    {
      ctx->function_localTable_ValuePtrs = 
          ctx->isSharedFunctions ? newDataBlock(ctx->function_localTable_Size[0] * sizeof(EEL_F *),8) : 
                                   newTmpBlock(ctx,ctx->function_localTable_Size[0] * sizeof(EEL_F *)); 
      if (!ctx->function_localTable_ValuePtrs)
      {
        ctx->function_localTable_Size[0]=0;
        function_numparms=0;
      }
      else
      {
        memset(ctx->function_localTable_ValuePtrs,0,sizeof(EEL_F *) * ctx->function_localTable_Size[0]); // force values to be allocated
      }
    }

    ctx->errVar=0;
    nseel_llinit(ctx);
    if (!nseel_yyparse(ctx,expr) && !ctx->errVar)
    {
      start_opcode = ctx->result;
    }
           
    if (start_opcode)
    {


#ifdef LOG_OPT
      char buf[512];
      sprintf(buf,"pre opt sz=%d\n",compileOpcodes(ctx,start_opcode,NULL,1024*1024*256,NULL, NULL));
#ifdef _WIN32
      OutputDebugString(buf);
#else
      printf("%s\n",buf);
#endif
#endif
      optimizeOpcodes(ctx,start_opcode);
#ifdef LOG_OPT
      sprintf(buf,"post opt sz=%d\n",compileOpcodes(ctx,start_opcode,NULL,1024*1024*256,NULL, NULL));
#ifdef _WIN32
      OutputDebugString(buf);
#else
      printf("%s\n",buf);
#endif
#endif

      startptr_size = compileOpcodes(ctx,start_opcode,NULL,1024*1024*256,NULL, NULL);

      if (is_fname[0])
      {
        _codeHandleFunctionRec *fr = ctx->isSharedFunctions ? newDataBlock(sizeof(_codeHandleFunctionRec),8) : 
                                        newTmpBlock(ctx,sizeof(_codeHandleFunctionRec)); 
        if (fr)
        {
          memset(fr,0,sizeof(_codeHandleFunctionRec));
          fr->startptr_size = startptr_size;
          fr->opcodes = start_opcode;
      
          fr->tmpspace_req = computTableTop;

          if (ctx->function_localTable_Size[0] > 0 && ctx->function_localTable_ValuePtrs)
          {
            fr->num_params=function_numparms;
            fr->localstorage = ctx->function_localTable_ValuePtrs;
            fr->localstorage_size = ctx->function_localTable_Size[0];
          }

          fr->usesThisPointer = ctx->function_usesThisPointer;
          fr->isCommonFunction = ctx->isSharedFunctions;

          strcpy(fr->fname,is_fname);

          if (ctx->isSharedFunctions)
          {
            fr->next = ctx->functions_common;
            ctx->functions_common = fr;
          }
          else
          {
            fr->next = ctx->functions_local;
            ctx->functions_local = fr;
          }         
        }
        continue;
      }

      if (!startptr_size) continue; // optimized away
      if (startptr_size>0)
      {
        startptr = newTmpBlock(ctx,startptr_size);
        if (startptr)
        {
          startptr_size=compileOpcodes(ctx,start_opcode,(unsigned char*)startptr,startptr_size,&computTableTop, NULL);
          if (startptr_size<=0) startptr = NULL;
          
        }
      }
    }

    memset(ctx->function_localTable_Size,0,sizeof(ctx->function_localTable_Size));
    memset(ctx->function_localTable_Names,0,sizeof(ctx->function_localTable_Names));
    ctx->function_localTable_ValuePtrs=0;
    ctx->function_usesThisPointer=0;
    
    if (!startptr) 
    { 
      int byteoffs = expr - expression_start;
      int destoffs,linenumber;
      char buf[21], *p;
      int x,le;
      
#ifdef NSEEL_EEL1_COMPAT_MODE
      if (!startptr) continue;
#endif

      if (ctx->errVar > 0) byteoffs += ctx->errVar;
      linenumber=findByteOffsetInSource(ctx,byteoffs,&destoffs);
      if (destoffs < 0) destoffs=0;

      le=strlen(_expression);
      if (destoffs >= le) destoffs=le;
      p= _expression + destoffs;
      for (x = 0;x < 20; x ++)
      {
	      if (!*p || *p == '\r' || *p == '\n') break;
	      buf[x]=*p++;
      }
      buf[x]=0;

      sprintf(ctx->last_error_string,"Around line %d '%s'",linenumber+lineoffs,buf);

      ctx->last_error_string[sizeof(ctx->last_error_string)-1]=0;
      startpts=NULL;
      startpts_tail=NULL; 
      had_err=1;
      break; 
    }
    
    if (!is_fname[0]) // redundant check (if is_fname[0] is set and we succeeded, it should continue)
                      // but we'll be on the safe side
    {
      topLevelCodeSegmentRec *p = newTmpBlock(ctx,sizeof(topLevelCodeSegmentRec));
      p->_next=0;
      p->code = startptr;
      p->codesz = startptr_size;
                  
      if (!startpts_tail) startpts_tail=startpts=p;
      else
      {
        startpts_tail->_next=p;
        startpts_tail=p;
      }

      if (curtabptr_sz < computTableTop)
      {
        curtabptr_sz=computTableTop;
      }
    }
  }
  free(ctx->compileLineRecs); ctx->compileLineRecs=0; ctx->compileLineRecs_size=0; ctx->compileLineRecs_alloc=0;

  if (handle->want_stack)
  {
    if (!handle->stack) startpts=NULL;
  }

  if (startpts) 
  {
    handle->workTable = curtabptr = newDataBlock((curtabptr_sz+64) * sizeof(EEL_F),32);
    if (!curtabptr) startpts=NULL;
  }

  ctx->tmpCodeHandle = NULL;

  if (startpts || (!had_err && (compile_flags & NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS)))
  {
    unsigned char *writeptr;
    topLevelCodeSegmentRec *p=startpts;
    int size=sizeof(GLUE_RET)+GLUE_FUNC_ENTER_SIZE+GLUE_FUNC_LEAVE_SIZE; // for ret at end :)

    // now we build one big code segment out of our list of them, inserting a mov esi, computable before each item
    while (p)
    {
      size += GLUE_RESET_WTP(NULL,0);
      size+=p->codesz;
      p=p->_next;
    }
    handle->code = newCodeBlock(size,32);
    if (handle->code)
    {
      writeptr=(unsigned char *)handle->code;
      memcpy(writeptr,&GLUE_FUNC_ENTER,GLUE_FUNC_ENTER_SIZE); writeptr += GLUE_FUNC_ENTER_SIZE;
      p=startpts;
      while (p)
      {
        int thissize=p->codesz;
        writeptr+=GLUE_RESET_WTP(writeptr,curtabptr);
        memcpy(writeptr,(char*)p->code,thissize);
        writeptr += thissize;
      
        p=p->_next;
      }
      memcpy(writeptr,&GLUE_FUNC_LEAVE,GLUE_FUNC_LEAVE_SIZE); writeptr += GLUE_FUNC_LEAVE_SIZE;
      memcpy(writeptr,&GLUE_RET,sizeof(GLUE_RET)); writeptr += sizeof(GLUE_RET);
      ctx->l_stats[1]=size;
    }
    
    handle->blocks = ctx->blocks_head;
    handle->blocks_data = ctx->blocks_head_data;
    ctx->blocks_head=0;
    ctx->blocks_head_data=0;
  }
  else
  {
    // failed compiling, or failed calloc()
    handle=NULL;              // return NULL (after resetting blocks_head)
  }


  ctx->functions_local = NULL;
  
  ctx->isSharedFunctions=0;

  freeBlocks(&ctx->tmpblocks_head);  // free blocks
  freeBlocks(&ctx->blocks_head);  // free blocks of code (will be nonzero only on error)
  freeBlocks(&ctx->blocks_head_data);  // free blocks of data (will be nonzero only on error)

  if (handle)
  {
    memcpy(handle->code_stats,ctx->l_stats,sizeof(ctx->l_stats));
    nseel_evallib_stats[0]+=ctx->l_stats[0];
    nseel_evallib_stats[1]+=ctx->l_stats[1];
    nseel_evallib_stats[2]+=ctx->l_stats[2];
    nseel_evallib_stats[3]+=ctx->l_stats[3];
    nseel_evallib_stats[4]++;
  }
  memset(ctx->l_stats,0,sizeof(ctx->l_stats));

  free(expression_start);

  return (NSEEL_CODEHANDLE)handle;
}

//------------------------------------------------------------------------------
void NSEEL_code_execute(NSEEL_CODEHANDLE code)
{
  INT_PTR tabptr;
  INT_PTR codeptr;
  codeHandleType *h = (codeHandleType *)code;
  if (!h || !h->code) return;

  codeptr = (INT_PTR) h->code;
#if 0
  {
	unsigned int *p=(unsigned int *)codeptr;
	while (*p != GLUE_RET[0])
	{
		printf("instr:%04X:%04X\n",*p>>16,*p&0xffff);
		p++;
	}
  }
#endif

  tabptr=(INT_PTR)h->workTable;
  //printf("calling code!\n");
  GLUE_CALL_CODE(tabptr,codeptr);

}


char *NSEEL_code_getcodeerror(NSEEL_VMCTX ctx)
{
  compileContext *c=(compileContext *)ctx;
  if (ctx && c->last_error_string[0]) return c->last_error_string;
  return 0;
}

//------------------------------------------------------------------------------
void NSEEL_code_free(NSEEL_CODEHANDLE code)
{
  codeHandleType *h = (codeHandleType *)code;
  if (h != NULL)
  {
    nseel_evallib_stats[0]-=h->code_stats[0];
    nseel_evallib_stats[1]-=h->code_stats[1];
    nseel_evallib_stats[2]-=h->code_stats[2];
    nseel_evallib_stats[3]-=h->code_stats[3];
    nseel_evallib_stats[4]--;
    
#ifdef EEL_PPC_NOFREECODE
  #pragma warn leaky-code mode, not freeing code, will leak, fixme!!!
#else
  freeBlocks(&h->blocks);
#endif
    
    freeBlocks(&h->blocks_data);


  }

}


//------------------------------------------------------------------------------
static void NSEEL_VM_freevars(NSEEL_VMCTX _ctx)
{
  if (_ctx)
  {
    compileContext *ctx=(compileContext *)_ctx;
    int x;
    if (ctx->varTable_Names || ctx->varTable_Values) for (x = 0; x < ctx->varTable_numBlocks; x ++)
    {
      if (ctx->varTable_Names) free(ctx->varTable_Names[x]);
      if (ctx->varTable_Values) free(ctx->varTable_Values[x]);
    }

    free(ctx->varTable_Values);
    free(ctx->varTable_Names);
    ctx->varTable_Values=0;
    ctx->varTable_Names=0;

    ctx->varTable_numBlocks=0;
  }
}


NSEEL_VMCTX NSEEL_VM_alloc() // return a handle
{
  compileContext *ctx=calloc(1,sizeof(compileContext));
  return ctx;
}

void NSEEL_VM_free(NSEEL_VMCTX _ctx) // free when done with a VM and ALL of its code have been freed, as well
{

  if (_ctx)
  {
    compileContext *ctx=(compileContext *)_ctx;
    NSEEL_VM_freevars(_ctx);
    NSEEL_VM_freeRAM(_ctx);

    freeBlocks(&ctx->tmpblocks_head);  // free blocks
    freeBlocks(&ctx->blocks_head);  // free blocks
    freeBlocks(&ctx->blocks_head_data);  // free blocks
    free(ctx->compileLineRecs);
    free(ctx);
  }

}

int *NSEEL_code_getstats(NSEEL_CODEHANDLE code)
{
  codeHandleType *h = (codeHandleType *)code;
  if (h)
  {
    return h->code_stats;
  }
  return 0;
}

void NSEEL_VM_SetCustomFuncThis(NSEEL_VMCTX ctx, void *thisptr)
{
  if (ctx)
  {
    compileContext *c=(compileContext*)ctx;
    c->caller_this=thisptr;
  }
}





void NSEEL_PProc_RAM(void *data, int data_size, compileContext *ctx)
{
  if (data_size>0) EEL_GLUE_set_immediate(data, ctx->ram_blocks); 
}
void NSEEL_PProc_RAM_freeblocks(void *data, int data_size, compileContext *ctx)
{
  if (data_size>0) EEL_GLUE_set_immediate(data, &ctx->ram_needfree); 
}

void NSEEL_PProc_THIS(void *data, int data_size, compileContext *ctx)
{
  if (data_size>0) EEL_GLUE_set_immediate(data, ctx->caller_this);
}
