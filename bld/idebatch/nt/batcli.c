/****************************************************************************
*
*                            Open Watcom Project
*
* Copyright (c) 2024      The Open Watcom Contributors. All Rights Reserved.
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


/*
        BATCLI :  NT interface to BATSERV
*/

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "batpipe.h"


static HANDLE   MemHdl;

const char *BatchLink( const char *name )
{
    if( name == NULL )
        name = DEFAULT_LINK_NAME;
    SemReadUp = OpenSemaphore( SEMAPHORE_ALL_ACCESS, FALSE, READUP_NAME );
    SemReadDone = OpenSemaphore( SEMAPHORE_ALL_ACCESS, FALSE, READDONE_NAME );
    SemWritten = OpenSemaphore( SEMAPHORE_ALL_ACCESS, FALSE, WRITTEN_NAME );
    MemHdl = OpenFileMapping( FILE_MAP_WRITE, FALSE, name );
    if( MemHdl == NULL || SemReadUp == NULL || SemWritten == NULL ) {
        return( "can not connect to batcher spawn server" );
    }
    SharedMemPtr = MapViewOfFile( MemHdl, FILE_MAP_WRITE, 0, 0, 0 );
    return( NULL );
}

int BatchMaxCmdLine( void )
{
    return( TRANS_DATA_MAXLEN - 1 );
}

batch_stat BatchChdir( const char *dir )
{
    BatservWriteData( LNK_CWD, dir, strlen( dir ) + 1 );
    BatservReadData();
    return( bdata.u.s.u.status );
}

int BatchSpawn( const char *cmd )
{
    BatservWriteData( LNK_RUN, cmd, strlen( cmd ) + 1 );
    return( 0 );
}

int BatchCollect( void *ptr, batch_len max, batch_stat *status )
{
    int         len;

    BatservWriteData( LNK_QUERY, &max, sizeof( max ) );
    len = BatservReadData();
    if( len > 0 ) {
        if( bdata.u.s.cmd == LNK_STATUS ) {
            *status = bdata.u.s.u.status;
            len = -1;
        } else {
            if( len > max )
                len = max;
            memcpy( ptr, bdata.u.s.u.data, len );
        }
    } else {
        len = 0;
    }
    return( len );
}

int BatchCancel( void )
{
    BatservWriteCmd( LNK_CANCEL );
    return( 0 );
}

int BatchAbort( void )
{
    BatservWriteCmd( LNK_ABORT );
    return( 0 );
}


void BatchUnlink( int shutdown )
{
    BatservWriteCmd( ( shutdown ) ? LNK_SHUTDOWN : LNK_DONE );
    CloseHandle( SemReadUp );
    CloseHandle( SemReadDone );
    CloseHandle( SemWritten );
    CloseHandle( MemHdl );
    UnmapViewOfFile( SharedMemPtr );
    SharedMemPtr = NULL;
}
