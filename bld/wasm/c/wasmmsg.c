/****************************************************************************
*
*                            Open Watcom Project
*
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
* Description:  WHEN YOU FIGURE OUT WHAT THIS FILE DOES, PLEASE
*               DESCRIBE IT HERE!
*
****************************************************************************/


#include "asmglob.h"

#include <fcntl.h>
#include <unistd.h>

#ifdef __WATCOMC__
  #include <conio.h>
  #include <process.h>
#endif


#if defined( USE_TEXT_MSGS )

typedef struct msgtxt {
    int     num;
    char    *text;
} msgtxt;

msgtxt txtmsgs[] = {
    #define pick(num,etext,jtext) {num,etext},
    #include "../h/asmshare.msg"
    #include "../h/womp.msg"
    #include "../h/wasm.msg"
    #include "usage.gh"
    #undef pick
};

#define MSG_NUM sizeof( txtmsgs ) / sizeof( txtmsgs[0] )
#define MSG_SIZE sizeof( txtmsgs[0] )

#else

#include "wressetr.h"
#include "wresset2.h"
#include "wreslang.h"

#define NIL_HANDLE      ((int)-1)
#define STDOUT_HANDLE   ((int)1)

#define NO_RES_MESSAGE  "Error: could not open message resource file.\r\n"
#define NO_RES_SIZE     (sizeof( NO_RES_MESSAGE ) - 1)

extern  long            FileShift;

static  HANDLE_INFO     hInstance = { 0 };
static  unsigned        MsgShift;

#endif

extern  int             trademark( void );
#ifdef __OSI__
extern char             *_Copyright;
#endif

#if defined( __UNIX__ )

void PrintfUsage( void )
{
    char        msg_buff[MAX_MESSAGE_SIZE];
    int         first_ln;

    trademark();
    first_ln = MSG_USAGE_BASE;
    for( first_ln++; ; first_ln++ ) {
        MsgGet( first_ln, msg_buff );
        if( ( msg_buff[ 0 ] == '.' ) && ( msg_buff[ 1 ] == 0 ) )
            break;
        puts( msg_buff );
    }
}

#else

static void con_output( const char *text )
{
    char c;

    do {
        c = *text;
        putchar( c );
        ++text;
    } while( *text );
    putchar( '\n' );
}

static void Wait_for_return( const char *page_text )
{
    if( isatty( fileno(stdout) ) ) {
        con_output( page_text );
        fflush( stdout );
        getch();
    }
}

void PrintfUsage( void )
{
    char        msg_buff[MAX_MESSAGE_SIZE];
    unsigned    count;
    char        page_text[MAX_MESSAGE_SIZE];
    int         first_ln;

    count = trademark();
#ifdef __OSI__
    if( _Copyright != NULL ) {
        puts( _Copyright );
        count += 1;
    }
#endif
    first_ln = MSG_USAGE_BASE;
    MsgGet( first_ln++, page_text );
    for( ; ; first_ln++ ) {
        if( ++count >= 23 ) {
            Wait_for_return( page_text );
            count = 0;
        }
        MsgGet( first_ln, msg_buff );
        if( ( msg_buff[ 0 ] == '.' ) && ( msg_buff[ 1 ] == 0 ) )
            break;
        puts( msg_buff );
    }
}

#endif

void MsgPrintf( int resourceid )
{
    char        msgbuf[MAX_MESSAGE_SIZE];

    if( !Options.banner_printed ) {
        Options.banner_printed = TRUE;
        trademark();
    }
    MsgGet( resourceid, msgbuf );
    printf( msgbuf );
}

void MsgPrintf1( int resourceid, char *token )
{
    char        msgbuf[MAX_MESSAGE_SIZE];

    if( !Options.banner_printed ) {
        Options.banner_printed = TRUE;
        trademark();
    }
    MsgGet( resourceid, msgbuf );
    printf( msgbuf, token );
}

#if defined( USE_TEXT_MSGS )

static int msg_cmp( const void *p1, const void *p2 )
{
    if( ((msgtxt *)p1)->num == ((msgtxt *)p2)->num )
        return( 0 );
    if( ((msgtxt *)p1)->num > ((msgtxt *)p2)->num )
        return( 1 );
    return( -1 );
}

#else

static off_t res_seek( int handle, off_t position, int where )
/* fool the resource compiler into thinking that the resource information
 * starts at offset 0 */
{
    if( where == SEEK_SET ) {
        return( lseek( handle, position + FileShift, where ) - FileShift );
    } else {
        return( lseek( handle, position, where ) );
    }
}

WResSetRtns( open, close, read, write, res_seek, tell, malloc, free );

#endif

int MsgInit( void )
{
#if !defined( USE_TEXT_MSGS )
    int         initerror;
    char        name[_MAX_PATH];

    hInstance.handle = NIL_HANDLE;
    if( _cmdname( name ) == NULL ) {
        initerror = 1;
    } else {
        hInstance.filename = name;
        OpenResFile( &hInstance );
        if( hInstance.handle == NIL_HANDLE ) {
            initerror = 1;
        } else {
            initerror = FindResources( &hInstance );
            if( !initerror ) {
                initerror = InitResources( &hInstance );
            }
        }
    }
    MsgShift = WResLanguage() * MSG_LANG_SPACING;
    if( !initerror && !MsgGet( MSG_USAGE_BASE, name ) ) {
        initerror = 1;
    }
    if( initerror ) {
        write( STDOUT_FILENO, NO_RES_MESSAGE, NO_RES_SIZE );
        MsgFini();
        return( 0 );
    }
#endif
    return( 1 );
}

void MsgFini( void )
{
#if !defined( USE_TEXT_MSGS )
    if( hInstance.handle != NIL_HANDLE ) {
        CloseResFile( &hInstance );
        hInstance.handle = NIL_HANDLE;
    }
#endif
}

int MsgGet( int id, char *buffer )
{
#if defined( USE_TEXT_MSGS )
    msgtxt  keyx;
    msgtxt  *result;

    keyx.num = id;
    result = bsearch( &keyx, txtmsgs, MSG_NUM, MSG_SIZE, msg_cmp );
    if( result != NULL ) {
        strncpy( buffer, result->text, MAX_MESSAGE_SIZE );
        buffer[MAX_MESSAGE_SIZE - 1] = '\0';
        return( 1 );
    }
#else
    if( LoadString( &hInstance, id + MsgShift, (LPSTR)buffer, MAX_MESSAGE_SIZE ) == 0 ) {
        return( 1 );
    }
#endif
    buffer[0] = '\0';
    return( 0 );
}
