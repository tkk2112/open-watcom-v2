/****************************************************************************
*
*                            Open Watcom Project
*
* Copyright (c) 2015-2022 The Open Watcom Contributors. All Rights Reserved.
*    Portions Copyright (c) 1983-2002 Sybase, Inc. All Rights Reserved.
*
*  ========================================================================
*
*    This file contains Original Code and/or Modifications of Original
*    Code as defined in and that are subject to the Sybase Open Watcom
*    Public License version 1.0 (the 'License'). You may not use this file
*    except in compliance with the License. BY USING THIS FILE YOU AGREE TO
*    ALL TERMS AND CONDITIONS OF THE LICENSE. A copy of the License is
*    provided with the Original Code and Modifications, and is also
*    available at www.sybase.com/developer/opensource.
*
*    The Original Code and all software distributed under the License are
*    distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
*    EXPRESS OR IMPLIED, AND SYBASE AND ALL CONTRIBUTORS HEREBY DISCLAIM
*    ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF
*    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR
*    NON-INFRINGEMENT. Please see the License for the specific language
*    governing rights and limitations under the License.
*
*  ========================================================================
*
* Description:  DOS real mode debugger access functions (16-bit code).
*
****************************************************************************/


//#define DEBUG_ME

#include <stdlib.h>
#include <string.h>
#include <i86.h>
#include "tinyio.h"
#include "dbg386.h"
#include "drset.h"
#include "exedos.h"
#include "exeos2.h"
#include "exephar.h"
#include "trperr.h"
#include "doserr.h"
#include "trpimp.h"
#include "trpcomm.h"
#include "ioports.h"
#include "winchk.h"
#include "madregs.h"
#include "x86cpu.h"
#include "miscx87.h"
#include "dosredir.h"
#include "brkptcpu.h"
#include "dosovl.h"
#include "dbgpsp.h"
#include "dospath.h"


typedef enum {
    EXE_UNKNOWN,
    EXE_DOS,                    /* DOS */
    EXE_OS2,                    /* OS/2 */
    EXE_PHARLAP_SIMPLE,         /* PharLap Simple */
    EXE_PHARLAP_EXTENDED_286,   /* PharLap Extended 286 */
    EXE_PHARLAP_EXTENDED_386,   /* PharLap Extended 386 (may be bound) */
    EXE_RATIONAL_386,           /* Rational DOS/4G app */
    EXE_LAST_TYPE
} EXE_TYPE;

/********************************************************************
 * NOTE: if you change this enum, you must update dbgtrap.asm
 */
typedef enum {
    F_Is386     = 0x01,
    F_IsMMX     = 0x02,
    F_IsXMM     = 0x04,
    F_DRsOn     = 0x08,
    F_Com_file  = 0x10,
    F_NoOvlMgr  = 0x20,
    F_BoundApp  = 0x40,
} FLAGS;
/********************************************************************/

#include "pushpck1.h"
typedef struct pblock {
    addr_seg    envstring;
    addr32_ptr  commandln;
    addr32_ptr  fcb01;
    addr32_ptr  fcb02;
    addr32_ptr  startsssp;
    addr32_ptr  startcsip;
} pblock;

/********************************************************************
 * NOTE: if you change this structure, you must update dbgtrap.asm
 */
typedef struct watch_point {
    addr32_ptr  addr;
    dword       value;
    dword       linear;
    word        dregs;
    word        len;
} watch_point;
/********************************************************************/
#include "poppck.h"

#define CMD_OFFSET      0x80

typedef enum {
    TRAP_SKIP = -1,
    TRAP_NONE,
    TRAP_TRACE_POINT,
    TRAP_BREAK_POINT,
    TRAP_WATCH_POINT,
    TRAP_USER,
    TRAP_TERMINATE,
    TRAP_MACH_EXCEPTION,
    TRAP_OVL_CHANGE_LOAD,
    TRAP_OVL_CHANGE_RET
} trap_types;

/* user modifiable flags */
#define USR_FLAGS (FLG_C | FLG_P | FLG_A | FLG_Z | FLG_S | FLG_I | FLG_D | FLG_O)

extern void MoveBytes( short, short, short, short, size_t );
/*  MoveBytes( fromseg, fromoff, toseg, tooff, len ); */
#pragma aux MoveBytes = \
        "rep movsb" \
    __parm __caller [__ds] [__si] [__es] [__di] [__cx] \
    __value         \
    __modify        [__si __di]

extern unsigned short MyFlags( void );
#pragma aux MyFlags = \
        "pushf"     \
        "pop  ax"   \
    __parm          [] \
    __value         [__ax] \
    __modify        []

extern tiny_ret_t       DOSLoadProg(char __far *, pblock __far *);
extern addr_seg         DOSTaskPSP(void);
extern void             EndUser(void);
extern unsigned_8       RunProg(trap_cpu_regs *, trap_cpu_regs *);
extern void             SetWatch386( unsigned, watch_point __far * );
extern void             SetWatchPnt(unsigned, watch_point __far *);
extern void             SetSingleStep(void);
extern void             SetSingle386(void);
extern void             InitVectors(void);
extern void             FiniVectors(void);
extern void             TrapTypeInit(void);
extern void             ClrIntVecs(void);
extern void             SetIntVecs(void);
extern void             DoRemInt(trap_cpu_regs *, unsigned);
extern char             Have87Emu(void);
extern void             Null87Emu( void );
extern void             Read87EmuState( void __far * );
extern void             Write87EmuState( void __far * );
extern unsigned         StringToFullPath( char * );
extern int              __far NoOvlsHdlr( int, void * );

extern word             __based(__segname("_CODE")) SegmentChain;

trap_cpu_regs           TaskRegs;
char                    DOS_major;
char                    DOS_minor;
bool                    BoundAppLoading;

FLAGS                   Flags;

#define MAX_WATCHES     32

static watch_point      WatchPoints[MAX_WATCHES];
static short            WatchCount;

static bool             IsBreak[4];

static word             NumSegments;
static opcode_type      BreakOpcode;
static addr48_ptr       BadBreak;
static bool             GotABadBreak;
static int              ExceptNum;
static unsigned_8       RealNPXType;
static unsigned_8       CPUType;

#ifdef DEBUG_ME
int out( char *str )
{
    char *p;

    p = str;
    while( *p != '\0' )
        ++p;
    TinyFarWrite( 1, str, p - str );
    return 0;
}

#define out0 out

static char hexbuff[80];

char *hex( unsigned long num )
{
    char *p;

    p = &hexbuff[79];
    *p = 0;
    if( num == 0 ) {
      *--p = '0';
      return( p );
    }
    while( num != 0 ) {
        *--p = "0123456789abcdef"[num & 15];
        num >>= 4;
    }
    return( p );

}
#else
    #define out( s )
    #define out0( s ) 0
    #define hex( n )
#endif


trap_retval TRAP_CORE( Get_sys_config )( void )
{
    get_sys_config_ret  *ret;

    ret = GetOutPtr( 0 );
    ret->sys.os = DIG_OS_DOS;
    ret->sys.osmajor = DOS_major;
    ret->sys.osminor = DOS_minor;
    ret->sys.cpu = CPUType;
    if( Have87Emu() ) {
        ret->sys.fpu = X86_EMU;
    } else if( RealNPXType != X86_NO ) {
        if( CPUType < X86_486 ) {
            ret->sys.fpu = RealNPXType;
        } else {
            ret->sys.fpu = CPUType & X86_CPU_MASK;
        }
    } else {
        ret->sys.fpu = X86_NO;
    }
    ret->sys.huge_shift = 12;
    ret->sys.arch = DIG_ARCH_X86;
    return( sizeof( *ret ) );
}


trap_retval TRAP_CORE( Map_addr )( void )
{
    word            seg;
    int             count;
    word            __far *segment;
    map_addr_req    *acc;
    map_addr_ret    *ret;

    acc = GetInPtr( 0 );
    ret = GetOutPtr( 0 );
    seg = acc->in_addr.segment;
    switch( seg ) {
    case MAP_FLAT_CODE_SELECTOR:
    case MAP_FLAT_DATA_SELECTOR:
        seg = 0;
        break;
    }
    if( Flags & F_BoundApp ) {
        segment = _MK_FP( SegmentChain, 14 );
        for( count = NumSegments - seg; count != 0; --count ) {
            segment = _MK_FP( *segment, 14 );
        }
        ret->out_addr.segment = _FP_SEG( segment ) + 1;
    } else {
        ret->out_addr.segment = DOSTaskPSP() + seg;
        if( (Flags & F_Com_file) == 0 ) {
            ret->out_addr.segment += 0x10;
        }
    }
    ret->out_addr.offset = acc->in_addr.offset;
    ret->lo_bound = 0;
    ret->hi_bound = ~(addr48_off)0;
    return( sizeof( *ret ) );
}

trap_retval TRAP_CORE( Machine_data )( void )
{
    machine_data_req    *acc;
    machine_data_ret    *ret;
    machine_data_spec   *data;

    acc = GetInPtr( 0 );
    ret = GetOutPtr( 0 );
    ret->cache_start = 0;
    ret->cache_end = ~(addr_off)0;
    if( acc->info_type == X86MD_ADDR_CHARACTERISTICS ) {
        data = GetOutPtr( sizeof( *ret ) );
        data->x86_addr_flags = X86AC_REAL;
        return( sizeof( *ret ) + sizeof( data->x86_addr_flags ) );
    }
    return( sizeof( *ret ) );
}

trap_retval TRAP_CORE( Checksum_mem )( void )
{
    unsigned char       __far *ptr;
    unsigned long       sum = 0;
    size_t              len;
    checksum_mem_req    *acc;
    checksum_mem_ret    *ret;

    acc = GetInPtr( 0 );
    ptr = _MK_FP( acc->in_addr.segment, acc->in_addr.offset );
    for( len = acc->len; len > 0; --len ) {
        sum += *ptr++;
    }
    ret = GetOutPtr( 0 );
    ret->result = sum;
    return( sizeof( *ret ) );
}


static bool IsInterrupt( addr48_ptr addr, size_t len )
{
    dword   start, end;

    start = ((dword)addr.segment << 4) + addr.offset;
    end = start + len;
    return( start < 0x400 || end < 0x400 );
}


trap_retval TRAP_CORE( Read_mem )( void )
{
    bool            int_tbl;
    read_mem_req    *acc;
    void            *data;
    size_t          len;

    acc = GetInPtr( 0 );
    data = GetOutPtr( 0 );
    acc->mem_addr.offset &= 0xffff;
    int_tbl = IsInterrupt( acc->mem_addr, acc->len );
    if( int_tbl )
        SetIntVecs();
    len = acc->len;
    if( ( acc->mem_addr.offset + len ) > 0xffff ) {
        len = 0x10000 - acc->mem_addr.offset;
    }
    MoveBytes( acc->mem_addr.segment, acc->mem_addr.offset,
               _FP_SEG( data ), _FP_OFF( data ), len );
    if( int_tbl )
        ClrIntVecs();
    return( len );
}


trap_retval TRAP_CORE( Write_mem )( void )
{
    bool            int_tbl;
    write_mem_req   *acc;
    write_mem_ret   *ret;
    size_t          len;
    void            *data;

    acc = GetInPtr( 0 );
    ret = GetOutPtr( 0 );
    data = GetInPtr( sizeof( *acc ) );
    len = GetTotalSizeIn() - sizeof( *acc );

    acc->mem_addr.offset &= 0xffff;
    int_tbl = IsInterrupt( acc->mem_addr, len );
    if( int_tbl )
        SetIntVecs();
    if( ( acc->mem_addr.offset + len ) > 0xffff ) {
        len = 0x10000 - acc->mem_addr.offset;
    }
    MoveBytes( _FP_SEG( data ), _FP_OFF( data ),
               acc->mem_addr.segment, acc->mem_addr.offset, len );
    if( int_tbl )
        ClrIntVecs();
    ret->len = len;
    return( sizeof( *ret ) );
}


trap_retval TRAP_CORE( Read_io )( void )
{
    read_io_req     *acc;
    void            *data;

    acc = GetInPtr( 0 );
    data = GetOutPtr( 0 );
    switch( acc->len ) {
    case 1:
        *(byte *)data = In_b( acc->IO_offset );
        break;
    case 2:
        *(word *)data = In_w( acc->IO_offset );
        break;
    case 4:
        if( Flags & F_Is386 ) {
            *(dword *)data = In_d( acc->IO_offset );
            break;
        }
        /* fall through */
    default:
        return( 0 );
    }
    return( acc->len );
}


trap_retval TRAP_CORE( Write_io )( void )
{
    write_io_req        *acc;
    write_io_ret        *ret;
    void                *data;
    size_t              len;

    acc = GetInPtr( 0 );
    data = GetInPtr( sizeof( *acc ) );
    len = GetTotalSizeIn() - sizeof( *acc );
    switch( len ) {
    case 1:
        Out_b( acc->IO_offset, *(byte *)data );
        break;
    case 2:
        Out_w( acc->IO_offset, *(word *)data );
        break;
    case 4:
        if( Flags & F_Is386 ) {
            Out_d( acc->IO_offset, *(dword *)data );
            break;
        }
        /* fall through */
    default:
        len = 0;
        break;
    }
    ret = GetOutPtr( 0 );
    ret->len = len;
    return( sizeof( *ret ) );
}

trap_retval TRAP_CORE( Read_regs )( void )
{
    mad_registers       *mr;

    mr = GetOutPtr( 0 );
    mr->x86.cpu = *(struct x86_cpu *)&TaskRegs;
    if( Have87Emu() ) {
        Read87EmuState( &mr->x86.u.fpu );
    } else if( RealNPXType != X86_NO ) {
        Read8087( &mr->x86.u.fpu );
    } else {
        memset( &mr->x86.u.fpu, 0, sizeof( mr->x86.u.fpu ) );
    }
    return( sizeof( mr->x86 ) );
}

trap_retval TRAP_CORE( Write_regs )( void )
{
    mad_registers       *mr;

    mr = GetInPtr( sizeof( write_regs_req ) );
    *(struct x86_cpu *)&TaskRegs = mr->x86.cpu;
    if( Have87Emu() ) {
        Write87EmuState( &mr->x86.u.fpu );
    } else if( RealNPXType != X86_NO ) {
        Write8087( &mr->x86.u.fpu );
    }
    return( 0 );
}

static EXE_TYPE CheckEXEType( tiny_handle_t handle )
{
    static dos_exe_header head;

    Flags &= ~F_Com_file;
    if( TINY_OK( TinyFarRead( handle, &head, sizeof( head ) ) ) ) {
        switch( head.signature ) {
        case SIMPLE_SIGNATURE:      // mp
        case REX_SIGNATURE:         // mq
        case EXTENDED_SIGNATURE:    // 'P3'
            return( EXE_PHARLAP_SIMPLE );
        case DOS_SIGNATURE:         // 'MZ'
            if( head.reloc_offset == OS2_EXE_HEADER_FOLLOWS )
                return( EXE_OS2 );
            return( EXE_DOS );
        default:
            Flags |= F_Com_file;
            break;
        }
    }
    return( EXE_UNKNOWN );
}

static size_t MergeArgvArray( const char *src, char __far *dst, size_t len )
{
    char    ch;
    char    __far *start = dst;

    while( len-- > 0 ) {
        ch = *src++;
        if( ch == '\0' ) {
            if( len == 0 )
                break;
            ch = ' ';
        }
        *dst++ = ch;
    }
    *dst = '\r';
    return( dst - start );
}

static opcode_type place_breakpoint_file( tiny_handle_t handle, dword pos )
{
    opcode_type old_opcode;

    TinySeek( handle, pos, TIO_SEEK_START );
    TinyFarRead( handle, &old_opcode, sizeof( old_opcode ) );
    TinySeek( handle, pos, TIO_SEEK_START );
    TinyFarWrite( handle, &BreakOpcode, sizeof( BreakOpcode ) );
    return( old_opcode );
}

static void remove_breakpoint_file( tiny_handle_t handle, dword pos, opcode_type old_opcode )
{
    TinySeek( handle, pos, TIO_SEEK_START );
    TinyFarWrite( handle, &old_opcode, sizeof( old_opcode ) );
}

static opcode_type place_breakpoint( addr48_ptr *addr )
{
    opcode_type __far *paddr;
    opcode_type old_opcode;

    paddr = MK_FP( addr->segment, addr->offset );
    old_opcode = *paddr;
    *paddr = BreakOpcode;
    if( *paddr != BreakOpcode ) {
        BadBreak = *addr;
        GotABadBreak = true;
    }
    return( old_opcode );
}

static void remove_breakpoint( addr48_ptr *addr, opcode_type old_opcode )
{
    opcode_type __far *paddr;

    paddr = MK_FP( addr->segment, addr->offset );
    if( *paddr == BreakOpcode ) {
        *paddr = old_opcode;
    }
    GotABadBreak = false;
}

trap_retval TRAP_CORE( Prog_load )( void )
{
    addr_seg        psp;
    pblock          parmblock;
    tiny_ret_t      rc;
    char            *parm;
    char            *name;
    char            exe_name[128];
    EXE_TYPE        exe;
    prog_load_ret   *ret;
    size_t          len;
    union {
        tiny_ret_t        rc;
        tiny_file_stamp_t stamp;
    } exe_time;
    word            value;
    opcode_type     old_opcode;
    os2_exe_header  os2_head;
    tiny_handle_t   handle;
    dword           NEOffset;
    dword           StartPos;
    dword           SegTable;
    addr48_ptr      start_addr;

    ExceptNum = -1;
    ret = GetOutPtr( 0 );
    ret->err = 0;
    memset( &TaskRegs, 0, sizeof( TaskRegs ) );
    TaskRegs.EFL = MyFlags() & ~USR_FLAGS;
    /* build a DOS command line parameter in our PSP command area */
    Flags &= ~F_BoundApp;
    psp = DbgPSP();
    name = GetInPtr( sizeof( prog_load_req ) );
    if( TINY_ERROR( FindFilePath( DIG_FILETYPE_EXE, name, exe_name ) ) ) {
        exe_name[0] = '\0';
    }
    parm = name;
    while( *parm++ != '\0' )        // skip program name
        {}
    len = GetTotalSizeIn() - sizeof( prog_load_req ) - ( parm - name );
    if( len > 126 )
        len = 126;
    *(byte __far *)_MK_FP( psp, CMD_OFFSET ) = MergeArgvArray( parm, _MK_FP( psp, CMD_OFFSET + 1 ), len );
    parmblock.envstring = 0;
    parmblock.commandln.segment = psp;
    parmblock.commandln.offset =  CMD_OFFSET;
    parmblock.fcb01.segment = psp;
    parmblock.fcb02.segment = psp;
    parmblock.fcb01.offset  = 0x5C;
    parmblock.fcb02.offset  = 0x6C;

    exe = EXE_UNKNOWN;
    rc = TinyOpen( exe_name, TIO_READ_WRITE );
    if( TINY_OK( rc ) ) {
        handle = TINY_INFO( rc );
        exe_time.rc = TinyGetFileStamp( handle );
        exe = CheckEXEType( handle );
        if( exe == EXE_OS2 ) {
            if( TINY_OK( TinySeek( handle, OS2_NE_OFFSET, TIO_SEEK_START ) )
              && TINY_OK( TinyFarRead( handle, &NEOffset, sizeof( NEOffset ) ) )
              && TINY_OK( TinySeek( handle, NEOffset, TIO_SEEK_START ) )
              && TINY_OK( TinyFarRead( handle, &os2_head, sizeof( os2_head ) ) ) ) {
                if( os2_head.signature == RAT_SIGNATURE_WORD ) {
                    exe = EXE_RATIONAL_386;
                } else if( os2_head.signature == OS2_SIGNATURE_WORD ) {
                    NumSegments = os2_head.segments;
                    SegTable = NEOffset + os2_head.segment_off;
                    if( os2_head.align == 0 )
                        os2_head.align = 9;
                    TinySeek( handle, SegTable + ( os2_head.entrynum - 1 ) * 8, TIO_SEEK_START );
                    TinyFarRead( handle, &value, sizeof( value ) );
                    StartPos = ( (dword)value << os2_head.align ) + os2_head.IP;
                    old_opcode = place_breakpoint_file( handle, StartPos );
                } else {
                    exe = EXE_UNKNOWN;
                }
            } else {
                exe = EXE_UNKNOWN;
            }
        }
        TinyClose( handle );
        switch( exe ) {
        case EXE_RATIONAL_386:
            ret->err = ERR_RATIONAL_EXE;
            return( sizeof( *ret ) );
        case EXE_PHARLAP_SIMPLE:
            ret->err = ERR_PHARLAP_EXE;
            return( sizeof( *ret ) );
        }
    }
    SegmentChain = 0;
    BoundAppLoading = false;
    rc = DOSLoadProg( exe_name, &parmblock );
    if( TINY_OK( rc ) ) {
        TaskRegs.SS = parmblock.startsssp.segment;
        /* for some insane reason DOS returns a starting SP two less then normal */
        TaskRegs.ESP = parmblock.startsssp.offset + 2;
        TaskRegs.CS = parmblock.startcsip.segment;
        TaskRegs.EIP = parmblock.startcsip.offset;
        psp = DOSTaskPSP();
    } else {
        psp = TinyAllocBlock( TinyAllocBlock( 0xffff ) );
        TinyFreeBlock( psp );
        TaskRegs.SS = psp + 0x10;
        TaskRegs.ESP = 0xfffe;
        TaskRegs.CS = psp + 0x10;
        TaskRegs.EIP = 0x100;
    }
    TaskRegs.DS = psp;
    TaskRegs.ES = psp;
    if( TINY_OK( rc ) ) {
        if( (Flags & F_NoOvlMgr) || !CheckOvl( parmblock.startcsip ) ) {
            if( exe == EXE_OS2 ) {
                BoundAppLoading = true;
                RunProg( &TaskRegs, &TaskRegs );
                start_addr.segment = TaskRegs.CS;
                start_addr.offset = TaskRegs.EIP;
                remove_breakpoint( &start_addr, old_opcode );
                BoundAppLoading = false;
                rc = TinyOpen( exe_name, TIO_READ_WRITE );
                if( TINY_OK( rc ) ) {
                    handle = TINY_INFO( rc );
                    remove_breakpoint_file( handle, StartPos, old_opcode );
                    TinySetFileStamp( handle, exe_time.stamp.time, exe_time.stamp.date );
                    TinyClose( handle );
                    Flags |= F_BoundApp;
                }
            }
        }
    }
    if( TINY_ERROR( rc ) )
        ret->err = TINY_INFO( rc );
    ret->task_id = psp;
    ret->flags = 0;
    ret->mod_handle = 0;
    return( sizeof( *ret ) );
}


trap_retval TRAP_CORE( Prog_kill )( void )
{
    prog_kill_ret       *ret;

out( "in AccKillProg\r\n" );
    ret = GetOutPtr( 0 );
    RedirectFini();
    if( DOSTaskPSP() != NULL ) {
out( "enduser\r\n" );
        EndUser();
out( "done enduser\r\n" );
    }
out( "null87emu\r\n" );
    Null87Emu();
    NullOvlHdlr();
    ExceptNum = -1;
    ret->err = 0;
out( "done AccKillProg\r\n" );
    return( sizeof( *ret ) );
}

static int DRegsCount( void )
{
    int     needed;
    int     i;

    needed = 0;
    for( i = 0; i < WatchCount; i++ ) {
        needed += WatchPoints[i].dregs;
    }
    return( needed );
}

trap_retval TRAP_CORE( Set_watch )( void )
{
    watch_point         *curr;
    set_watch_req       *wp;
    set_watch_ret       *wr;

    wp = GetInPtr( 0 );
    wr = GetOutPtr( 0 );
    wr->err = 1;
    wr->multiplier = 0;
    if( WatchCount < MAX_WATCHES ) {
        wr->err = 0;
        curr = WatchPoints + WatchCount;
        curr->addr.segment = wp->watch_addr.segment;
        curr->addr.offset = wp->watch_addr.offset;
        curr->value = *(dword __far *)_MK_FP( wp->watch_addr.segment, wp->watch_addr.offset );
        curr->linear = ( (dword)wp->watch_addr.segment << 4 ) + wp->watch_addr.offset;
        curr->len = wp->size;
        curr->linear &= ~( curr->len - 1 );
        curr->dregs = ( wp->watch_addr.offset & ( curr->len - 1 ) ) ? 2 : 1;
        ++WatchCount;
        if( Flags & F_DRsOn ) {
            if( DRegsCount() <= 4 ) {
                wr->multiplier |= USING_DEBUG_REG;
            }
        }
    }
    wr->multiplier |= 200;
    return( sizeof( *wr ) );
}

trap_retval TRAP_CORE( Clear_watch )( void )
{
    WatchCount = 0;
    return( 0 );
}

trap_retval TRAP_CORE( Set_break )( void )
{
    set_break_req   *acc;
    set_break_ret   *ret;

    acc = GetInPtr( 0 );
    ret = GetOutPtr( 0 );
    ret->old = place_breakpoint( &acc->break_addr );
    return( sizeof( *ret ) );
}


trap_retval TRAP_CORE( Clear_break )( void )
{
    clear_break_req     *acc;

    acc = GetInPtr( 0 );
    remove_breakpoint( &acc->break_addr, acc->old );
    return( 0 );
}

static unsigned long SetDRn( int i, unsigned long linear, long type )
{
    switch( i ) {
    case 0:
        SetDR0( linear );
        break;
    case 1:
        SetDR1( linear );
        break;
    case 2:
        SetDR2( linear );
        break;
    case 3:
        SetDR3( linear );
        break;
    }
    return( ( type << DR7_RWLSHIFT(i) )
//          | ( DR7_GEMASK << DR7_GLSHIFT(i) ) | DR7_GE
          | ( DR7_LEMASK << DR7_GLSHIFT(i) ) | DR7_LE );
}


static int ClearDebugRegs( int trap )
{
    long        dr6;
    int         i;

    if( Flags & F_DRsOn ) {
        out( "tr=" ); out( hex( trap ) );
        out( " dr6=" ); out( hex( GetDR6() ) );
        out( "\r\n" );
        if( trap == TRAP_WATCH_POINT ) { /* could be a 386 break point */
            dr6 = GetDR6();
            if( ( ( dr6 & DR6_B0 ) && IsBreak[0] )
             || ( ( dr6 & DR6_B1 ) && IsBreak[1] )
             || ( ( dr6 & DR6_B2 ) && IsBreak[2] )
             || ( ( dr6 & DR6_B3 ) && IsBreak[3] ) ) {
                 trap = TRAP_BREAK_POINT;
             }
        }
        for( i = 0; i < 4; ++i ) {
            IsBreak[i] = false;
        }
        SetDR6( 0 );
        SetDR7( 0 );
    }
    return( trap );
}


static bool SetDebugRegs( void )
{
    int                 i;
    int                 dr;
    unsigned long       dr7;
    unsigned long       linear;
    bool                watch386;

    if( (Flags & F_DRsOn) == 0 )
        return( false );
    dr  = 0;
    dr7 = 0;
    if( DRegsCount() > 4 ) {
        watch386 = false;
    } else {
        for( i = 0; i < WatchCount; ++i ) {
            dr7 |= SetDRn( dr, WatchPoints[i].linear, DRLen( WatchPoints[i].len ) | DR7_BWR );
            ++dr;
            if( WatchPoints[i].dregs == 2 ) {
                dr7 |= SetDRn( dr, WatchPoints[i].linear + 4, DRLen( WatchPoints[i].len ) | DR7_BWR );
                ++dr;
            }
            watch386 = true;
        }
    }
    if( GotABadBreak && dr < 4 ) {
        linear = ( (unsigned long)BadBreak.segment << 4 ) + BadBreak.offset;
        dr7 |= SetDRn( dr, linear, DR7_L1 | DR7_BINST );
        IsBreak[dr] = true;
        ++dr;
    }
    SetDR7( dr7 );
    return( watch386 );
}

static trap_conditions MapReturn( int trap )
{
    out( "cond=" );
    switch( trap ) {
    case TRAP_TRACE_POINT:
        out( "trace point" );
        return( COND_TRACE );
    case TRAP_BREAK_POINT:
        out( "break point" );
        return( COND_BREAK );
    case TRAP_WATCH_POINT:
        out( "watch point" );
        return( COND_WATCH );
    case TRAP_USER:
        out( "user" );
        return( COND_USER );
    case TRAP_TERMINATE:
        out( "terminate" );
        return( COND_TERMINATE );
    case TRAP_MACH_EXCEPTION:
        out( "exception" );
        ExceptNum = 0;
        return( COND_EXCEPTION );
    case TRAP_OVL_CHANGE_LOAD:
        out( "overlay load" );
        return( COND_SECTIONS );
    case TRAP_OVL_CHANGE_RET:
        out( "overlay ret" );
        return( COND_SECTIONS );
    default:
        break;
    }
    out( "none" );
    return( 0 );
}

static trap_elen ProgRun( bool step )
{
    bool        watch386;
    prog_go_ret *ret;

    ret = GetOutPtr( 0 );
    if( Flags & F_DRsOn ) {
        SetSingle386();
    } else {
        SetSingleStep();
    }
    if( step ) {
        TaskRegs.EFL |= FLG_T;
    } else  {
        watch386 = SetDebugRegs();
        if( WatchCount != 0 && !watch386 ) {
            if( Flags & F_DRsOn ) {
                SetWatch386( WatchCount, WatchPoints );
            } else {
                SetWatchPnt( WatchCount, WatchPoints );
            }
            TaskRegs.EFL |= FLG_T;
        }
    }
    out( "in CS:EIP=" ); out( hex( TaskRegs.CS ) ); out(":" ); out( hex( TaskRegs.EIP ) );
    out( " SS:ESP=" ); out( hex( TaskRegs.SS ) ); out(":" ); out( hex( TaskRegs.ESP ) );
    out( "\r\n" );
    ret->conditions = MapReturn( ClearDebugRegs( RunProg( &TaskRegs, &TaskRegs ) ) );
    ret->conditions |= COND_CONFIG;
//    out( "cond=" ); out( hex( ret->conditions ) );
    out( " CS:EIP=" ); out( hex( TaskRegs.CS ) ); out(":" ); out( hex( TaskRegs.EIP ) );
    out( " SS:ESP=" ); out( hex( TaskRegs.SS ) ); out(":" ); out( hex( TaskRegs.ESP ) );
    out( "\r\n" );
    ret->stack_pointer.segment = TaskRegs.SS;
    ret->stack_pointer.offset  = TaskRegs.ESP;
    ret->program_counter.segment = TaskRegs.CS;
    ret->program_counter.offset  = TaskRegs.EIP;
    TaskRegs.EFL &= ~FLG_T;
    WatchCount = 0;
    return( sizeof( *ret ) );
}

trap_retval TRAP_CORE( Prog_go )( void )
{
    return( ProgRun( false ) );
}

trap_retval TRAP_CORE( Prog_step )( void )
{
    return( ProgRun( true ) );
}

trap_retval TRAP_CORE( Get_next_alias )( void )
{
    get_next_alias_ret  *ret;

    ret = GetOutPtr( 0 );
    ret->seg = 0;
    ret->alias = 0;
    return( sizeof( *ret ) );
}

trap_retval TRAP_CORE( Get_lib_name )( void )
{
    get_lib_name_ret    *ret;

    ret = GetOutPtr( 0 );
    ret->mod_handle = 0;
    return( sizeof( *ret ) );
}

trap_retval TRAP_CORE( Get_err_text )( void )
{
    static const char *const DosErrMsgs[] = {
        #define pick( a, b )    b,
        #include "dosmsgs.h"
        #undef pick
    };
    get_err_text_req    *acc;
    char                *err_txt;

    acc = GetInPtr( 0 );
    err_txt = GetOutPtr( 0 );
    if( acc->err < ERR_LAST ) {
        strcpy( err_txt, DosErrMsgs[acc->err] );
    } else {
        strcpy( err_txt, TRP_ERR_unknown_system_error );
        ultoa( acc->err, err_txt + strlen( err_txt ), 16 );
    }
    return( strlen( err_txt ) + 1 );
}

trap_retval TRAP_CORE( Get_message_text )( void )
{
    static const char * const ExceptionMsgs[] = {
        #define pick(a,b) b,
        #include "x86exc.h"
        #undef pick
    };
    get_message_text_ret        *ret;
    char                        *err_txt;

    ret = GetOutPtr( 0 );
    err_txt = GetOutPtr( sizeof( *ret ) );
    if( ExceptNum == -1 ) {
        err_txt[0] = '\0';
    } else {
        if( ExceptNum < sizeof( ExceptionMsgs ) / sizeof( ExceptionMsgs[0] ) ) {
            strcpy( err_txt, ExceptionMsgs[ExceptNum] );
        } else {
            strcpy( err_txt, TRP_EXC_unknown );
        }
        ExceptNum = -1;
    }
    ret->flags = MSG_NEWLINE | MSG_ERROR;
    return( sizeof( *ret ) + strlen( err_txt ) + 1 );
}

trap_version TRAPENTRY TrapInit( const char *parms, char *err, bool remote )
{
    trap_version ver;

    /* unused parameters */ (void)remote;

out( "in TrapInit\r\n" );
out( "    checking environment:\r\n" );
    Flags = 0;
    CPUType = X86CPUType();
    if( CPUType >= X86_386 )
        Flags |= F_Is386;
    if( *parms == 'D' || *parms == 'd' ) {
        ++parms;
    } else if( out0( "    CPU type\r\n" ) || ( CPUType < X86_386 ) ) {
    } else if( out0( "    WinEnh\r\n" ) || ( EnhancedWinCheck() & 0x7f ) ) {
        /* Enhanced Windows 3.0 VM kernel messes up handling of debug regs */
    } else if( out0( "    DOSEMU\r\n" ) || DOSEMUCheck() ) {
        /* no fiddling with debug regs in Linux DOSEMU either */
    } else {
        Flags |= F_DRsOn;
    }
    if( *parms == 'O' || *parms == 'o' ) {
        Flags |= F_NoOvlMgr;
    }
out( "    done checking environment\r\n" );
    err[0] = '\0'; /* all ok */

    if( CPUType & X86_MMX )
        Flags |= F_IsMMX;
    if( CPUType & X86_XMM )
        Flags |= F_IsXMM;

    /* NPXType initializes '87, so check for it before a program
       starts using the thing */
    RealNPXType = NPXType();
    InitVectors();
    if( DOS_major >= 20 ) {
        /* In an OS/2 2.0 DOS box. It doesn't let us fiddle the debug
        registers. The check is done here because InitVectors is the
        routine that sets up DOS_major */
        Flags &= ~F_DRsOn;
    }
    Null87Emu();
    NullOvlHdlr();
    TrapTypeInit();
    RedirectInit();
    ExceptNum = -1;
    WatchCount = 0;
    BreakOpcode = BRKPOINT;
    ver.major = TRAP_MAJOR_VERSION;
    ver.minor = TRAP_MINOR_VERSION;
    ver.remote = false;
out( "done TrapInit\r\n" );
    return( ver );
}

void TRAPENTRY TrapFini( void )
{
out( "in TrapFini\r\n" );
    FiniVectors();
out( "done TrapFini\r\n" );
}
