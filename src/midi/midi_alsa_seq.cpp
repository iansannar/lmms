/*
 * midi_alsa_seq.cpp - ALSA-sequencer-client
 *
 * Copyright (c) 2005 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * 
 * This file is part of Linux MultiMedia Studio - http://lmms.sourceforge.net
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */


#include "qt3support.h"

#ifdef QT4

#include <QLineEdit>
#include <QLabel>

#else

#include <qpair.h>
#include <qlineedit.h>
#include <qlabel.h>

#endif


#include "midi_alsa_seq.h"
#include "config_mgr.h"
#include "gui_templates.h"
#include "song_editor.h"
#include "midi_port.h"
#include "note.h"


#ifdef ALSA_SUPPORT


midiALSASeq::midiALSASeq( void ) :
#ifndef QT4
	QObject(),
#endif
	midiClient(),
	QThread(),
	m_seqHandle( NULL ),
	m_queueID( -1 ),
	m_quit( FALSE ),
	m_portListUpdateTimer( this ),
	m_readablePorts(),
	m_writeablePorts()
{
	int err;
	if( ( err = snd_seq_open( &m_seqHandle,
#ifdef QT4
					probeDevice().toAscii().constData(),
#else
					probeDevice().ascii(),
#endif
						SND_SEQ_OPEN_DUPLEX, 0 ) ) < 0 )
	{
		printf( "cannot open sequencer: %s\n", snd_strerror( err ) );
		return;
	}
	snd_seq_set_client_name( m_seqHandle, "LMMS" );


	m_queueID = snd_seq_alloc_queue( m_seqHandle );
	snd_seq_queue_tempo_t * tempo;
	snd_seq_queue_tempo_alloca( &tempo );
	snd_seq_queue_tempo_set_tempo( tempo, 6000000 /
						songEditor::inst()->getBPM() );
	snd_seq_queue_tempo_set_ppq( tempo, 16 );
	snd_seq_set_queue_tempo( m_seqHandle, m_queueID, tempo );

	snd_seq_start_queue( m_seqHandle, m_queueID, NULL );
	changeQueueTempo( songEditor::inst()->getBPM() );
	connect( songEditor::inst(), SIGNAL( bpmChanged( int ) ),
			this, SLOT( changeQueueTempo( int ) ) );

	// initial list-update
	updatePortList();

	connect( &m_portListUpdateTimer, SIGNAL( timeout() ),
					this, SLOT( updatePortList() ) );
	// we check for port-changes every second
	m_portListUpdateTimer.start( 1000 );

	start( 
#if QT_VERSION >= 0x030200	
	    	QThread::LowPriority 
#endif
					);
}




midiALSASeq::~midiALSASeq()
{
	if( running() )
	{
		m_quit = TRUE;
		wait( 500 );
		terminate();

		snd_seq_stop_queue( m_seqHandle, m_queueID, NULL );
		snd_seq_free_queue( m_seqHandle, m_queueID );
		snd_seq_close( m_seqHandle );
	}
}




QString midiALSASeq::probeDevice( void )
{
	QString dev = configManager::inst()->value( "midialsaseq", "device" );
	if( dev == "" )
	{
		if( getenv( "MIDIDEV" ) != NULL )
		{
			return( getenv( "MIDIDEV" ) );
		}
		return( "default" );
	}
	return( dev );
}




void midiALSASeq::processOutEvent( const midiEvent & _me,
						const midiTime & _time,
						const midiPort * _port )
{
	// HACK!!! - need a better solution which isn't that easy since we
	// cannot store const-ptrs in our map because we need to call non-const
	// methods of MIDI-port - it's a mess...
	midiPort * p = const_cast<midiPort *>( _port );

	snd_seq_event_t ev;
	snd_seq_ev_clear( &ev );
	snd_seq_ev_set_source( &ev, ( m_portIDs[p][1] != -1 ) ?
					m_portIDs[p][1] : m_portIDs[p][0] );
	snd_seq_ev_set_subs( &ev );
	snd_seq_ev_schedule_tick( &ev, m_queueID, 1,
						static_cast<Sint32>( _time ) );
	ev.queue =  m_queueID;
	switch( _me.m_type )
	{
		case NOTE_ON:
			snd_seq_ev_set_noteon( &ev,
						_port->outputChannel(),
						_me.key() + NOTES_PER_OCTAVE,
						_me.velocity() );
			break;

		case NOTE_OFF:
			snd_seq_ev_set_noteoff( &ev,
						_port->outputChannel(),
						_me.key() + NOTES_PER_OCTAVE,
						_me.velocity() );
			break;

		case KEY_PRESSURE:
			snd_seq_ev_set_keypress( &ev,
						_port->outputChannel(),
						_me.key() + NOTES_PER_OCTAVE,
						_me.velocity() );
			break;

		case PITCH_BEND:
			snd_seq_ev_set_pitchbend( &ev,
						_port->outputChannel(),
						_me.m_data.m_param[0] - 8192 );
			break;

		case PROGRAM_CHANGE:
			snd_seq_ev_set_pgmchange( &ev,
						_port->outputChannel(),
						_me.m_data.m_param[0] );
			break;

		case CHANNEL_PRESSURE:
			snd_seq_ev_set_chanpress( &ev,
						_port->outputChannel(),
						_me.m_data.m_param[0] );
			break;

		default:
			printf( "ALSA-sequencer: unhandled output event %d\n",
							(int) _me.m_type );
			return;
	}

	snd_seq_event_output( m_seqHandle, &ev );
	snd_seq_drain_output( m_seqHandle );

}




void midiALSASeq::applyPortMode( midiPort * _port )
{
	// determine port-capabilities
	unsigned int caps[2] = { 0, 0 };

	switch( _port->mode() )
	{
		case midiPort::DUPLEX:
			caps[1] |= SND_SEQ_PORT_CAP_READ |
						SND_SEQ_PORT_CAP_SUBS_READ;

		case midiPort::INPUT:
			caps[0] |= SND_SEQ_PORT_CAP_WRITE |
						SND_SEQ_PORT_CAP_SUBS_WRITE;
			break;

		case midiPort::OUTPUT:
			caps[0] |= SND_SEQ_PORT_CAP_READ |
						SND_SEQ_PORT_CAP_SUBS_READ;
			break;

		default:
			break;
	}

	for( int i = 0; i < 2; ++i )
	{
		if( caps[i] != 0 )
		{
			// no port there yet?
			if( m_portIDs[_port][i] == -1 )
			{
				// then create one;
				m_portIDs[_port][i] =
						snd_seq_create_simple_port(
							m_seqHandle,
							_port->name().ascii(),
							caps[i],
						SND_SEQ_PORT_TYPE_MIDI_GENERIC |
						SND_SEQ_PORT_TYPE_APPLICATION );
				continue;
			}
			// this C-API sucks!! normally we at least could create
			// a local snd_seq_port_info_t variable but the type-
			// info for this is hidden and we have to mess with
			// pointers...
			snd_seq_port_info_t * port_info;
			snd_seq_port_info_malloc( &port_info );
			snd_seq_get_port_info( m_seqHandle, m_portIDs[_port][i],
							port_info );
			snd_seq_port_info_set_capability( port_info, caps[i] );
			snd_seq_set_port_info( m_seqHandle, m_portIDs[_port][i],
							port_info );
			snd_seq_port_info_free( port_info );
		}
		// still a port there although no caps? ( = dummy port)
		else if( m_portIDs[_port][i] != -1 )
		{
			// then remove this port
			snd_seq_delete_port( m_seqHandle, m_portIDs[_port][i] );
			m_portIDs[_port][i] = -1;
		}
	}

}




void midiALSASeq::applyPortName( midiPort * _port )
{
	for( int i = 0; i < 2; ++i )
	{
		if( m_portIDs[_port][i] == -1 )
		{
			continue;
		}
		// this C-API sucks!! normally we at least could create a local
		// snd_seq_port_info_t variable but the type-info for this is
		// hidden and we have to mess with pointers...
		snd_seq_port_info_t * port_info;
		snd_seq_port_info_malloc( &port_info );
		snd_seq_get_port_info( m_seqHandle, m_portIDs[_port][i],
							port_info );
		snd_seq_port_info_set_name( port_info,
						_port->name().ascii() );
		snd_seq_set_port_info( m_seqHandle, m_portIDs[_port][i],
							port_info );
		snd_seq_port_info_free( port_info );
	}
	// this small workaround would make qjackctl refresh it's MIDI-
	// connection-window since it doesn't update it automatically if only
	// the name of a client-port changes
/*	snd_seq_delete_simple_port( m_seqHandle,
			snd_seq_create_simple_port( m_seqHandle, "", 0,
					SND_SEQ_PORT_TYPE_APPLICATION ) );*/
}




void midiALSASeq::removePort( midiPort * _port )
{
	if( m_portIDs.contains( _port ) )
	{
		snd_seq_delete_port( m_seqHandle, m_portIDs[_port][0] );
		snd_seq_delete_port( m_seqHandle, m_portIDs[_port][1] );
		m_portIDs.remove( _port );
	}
	midiClient::removePort( _port );
}




void midiALSASeq::subscribeReadablePort( midiPort * _port,
						const QString & _dest,
						bool _unsubscribe )
{
	if( m_portIDs.contains( _port ) == FALSE ||
		( _port->mode() != midiPort::INPUT &&
		  _port->mode() != midiPort::DUPLEX ) )
	{
		return;
	}
	snd_seq_addr_t sender;
	if( snd_seq_parse_address( m_seqHandle, &sender,
					_dest.section( ' ', 0, 0 ).ascii() ) )
	{
		printf( "error parsing sender-address!!\n" );
		return;
	}
	snd_seq_port_info_t * port_info;
	snd_seq_port_info_malloc( &port_info );
	snd_seq_get_port_info( m_seqHandle, m_portIDs[_port][0], port_info );
	const snd_seq_addr_t * dest = snd_seq_port_info_get_addr( port_info );
	snd_seq_port_subscribe_t * subs;
	snd_seq_port_subscribe_alloca( &subs );
	snd_seq_port_subscribe_set_sender( subs, &sender );
	snd_seq_port_subscribe_set_dest( subs, dest );
	if( _unsubscribe )
	{
		snd_seq_unsubscribe_port( m_seqHandle, subs );
	}
	else
	{
		snd_seq_subscribe_port( m_seqHandle, subs );
	}
	snd_seq_port_info_free( port_info );
}




void midiALSASeq::subscribeWriteablePort( midiPort * _port,
						const QString & _dest,
						bool _unsubscribe )
{
	if( m_portIDs.contains( _port ) == FALSE ||
		( _port->mode() != midiPort::OUTPUT &&
		  _port->mode() != midiPort::DUPLEX ) )
	{
		return;
	}
	snd_seq_addr_t dest;
	if( snd_seq_parse_address( m_seqHandle, &dest,
					_dest.section( ' ', 0, 0 ).ascii() ) )
	{
		printf( "error parsing dest-address!!\n" );
		return;
	}
	snd_seq_port_info_t * port_info;
	snd_seq_port_info_malloc( &port_info );
	snd_seq_get_port_info( m_seqHandle, ( m_portIDs[_port][1] == -1 ) ?
						m_portIDs[_port][0] :
						m_portIDs[_port][1],
						port_info );
	const snd_seq_addr_t * sender = snd_seq_port_info_get_addr( port_info );
	snd_seq_port_subscribe_t * subs;
	snd_seq_port_subscribe_alloca( &subs );
	snd_seq_port_subscribe_set_sender( subs, sender );
	snd_seq_port_subscribe_set_dest( subs, &dest );
	if( _unsubscribe )
	{
		snd_seq_unsubscribe_port( m_seqHandle, subs );
	}
	else
	{
		snd_seq_subscribe_port( m_seqHandle, subs );
	}
	snd_seq_port_info_free( port_info );
}




void midiALSASeq::run( void )
{
	while( m_quit == FALSE )
	{
		snd_seq_event_t * ev;
		snd_seq_event_input( m_seqHandle, &ev );

		midiPort * dest = NULL;
		for( csize i = 0; i < m_portIDs.values().size(); ++i )
		{
			if( m_portIDs.values()[i][0] == ev->dest.port ||
				m_portIDs.values()[i][0] == ev->dest.port )
			{
				dest = m_portIDs.keys()[i];
			}
		}

		if( dest == NULL )
		{
			continue;
		}

		switch( ev->type )
		{
			case SND_SEQ_EVENT_NOTEON:
				dest->processInEvent( midiEvent( NOTE_ON,
							ev->data.note.channel,
							ev->data.note.note -
							NOTES_PER_OCTAVE,
							ev->data.note.velocity
							),
						midiTime( ev->time.tick ) );
				break;

			case SND_SEQ_EVENT_NOTEOFF:
				dest->processInEvent( midiEvent( NOTE_OFF,
							ev->data.note.channel,
							ev->data.note.note -
							NOTES_PER_OCTAVE,
							ev->data.note.velocity
							),
						midiTime( ev->time.tick) );
				break;

			case SND_SEQ_EVENT_KEYPRESS:
				dest->processInEvent( midiEvent( KEY_PRESSURE,
							ev->data.note.channel,
							ev->data.note.note -
							NOTES_PER_OCTAVE,
							ev->data.note.velocity
							), midiTime() );
				break;

			case SND_SEQ_EVENT_SENSING:
			case SND_SEQ_EVENT_CLOCK:
				break;

			default:
				printf( "ALSA-sequencer: unhandled input "
						"event %d\n", ev->type );
				break;
		}

	}

}




void midiALSASeq::changeQueueTempo( int _bpm )
{
	snd_seq_change_queue_tempo( m_seqHandle, m_queueID, 60000000 / _bpm,
									NULL );
	snd_seq_drain_output( m_seqHandle );
}




void midiALSASeq::updatePortList( void )
{
	QStringList readable_ports;
	QStringList writeable_ports;

	// get input- and output-ports
	snd_seq_client_info_t * cinfo;
	snd_seq_port_info_t * pinfo;

	snd_seq_client_info_alloca( &cinfo );
	snd_seq_port_info_alloca( &pinfo );

	snd_seq_client_info_set_client( cinfo, -1 );
	while( snd_seq_query_next_client( m_seqHandle, cinfo ) >= 0 )
	{
		int client = snd_seq_client_info_get_client( cinfo );

		snd_seq_port_info_set_client( pinfo, client );
		snd_seq_port_info_set_port( pinfo, -1 );
		while( snd_seq_query_next_port( m_seqHandle, pinfo ) >= 0 )
		{
			// we need both READ and SUBS_READ
			if( ( snd_seq_port_info_get_capability( pinfo )
			     & ( SND_SEQ_PORT_CAP_READ |
					SND_SEQ_PORT_CAP_SUBS_READ ) ) ==
					( SND_SEQ_PORT_CAP_READ |
					  	SND_SEQ_PORT_CAP_SUBS_READ ) )
			{
				readable_ports.push_back( 
					QString( "%1:%2 %3:%4" ).
					arg( snd_seq_port_info_get_client(
								pinfo ) ).
					arg( snd_seq_port_info_get_port(
								pinfo ) ).
					arg( snd_seq_client_info_get_name(
								cinfo ) ).
					arg( snd_seq_port_info_get_name(
								pinfo ) ) );
			}
			if( ( snd_seq_port_info_get_capability( pinfo )
			     & ( SND_SEQ_PORT_CAP_WRITE |
					SND_SEQ_PORT_CAP_SUBS_WRITE ) ) ==
					( SND_SEQ_PORT_CAP_WRITE |
					  	SND_SEQ_PORT_CAP_SUBS_WRITE ) )
			{
				writeable_ports.push_back( 
					QString( "%1:%2 %3:%4" ).
					arg( snd_seq_port_info_get_client(
								pinfo ) ).
					arg( snd_seq_port_info_get_port(
								pinfo ) ).
					arg( snd_seq_client_info_get_name(
								cinfo ) ).
					arg( snd_seq_port_info_get_name(
								pinfo ) ) );
			}
		}
	}

/*	snd_seq_client_info_free( cinfo );
	snd_seq_port_info_free( pinfo );*/

	if( m_readablePorts != readable_ports )
	{
		m_readablePorts = readable_ports;
		emit( readablePortsChanged() );
	}

	if( m_writeablePorts != writeable_ports )
	{
		m_writeablePorts = writeable_ports;
		emit( writeablePortsChanged() );
	}
}







midiALSASeq::setupWidget::setupWidget( QWidget * _parent ) :
	midiClient::setupWidget( midiALSASeq::name(), _parent )
{
	m_device = new QLineEdit( midiALSASeq::probeDevice(), this );
	m_device->setGeometry( 10, 20, 160, 20 );

	QLabel * dev_lbl = new QLabel( tr( "DEVICE" ), this );
	dev_lbl->setFont( pointSize<6>( dev_lbl->font() ) );
	dev_lbl->setGeometry( 10, 40, 160, 10 );
}




midiALSASeq::setupWidget::~setupWidget()
{
}




void midiALSASeq::setupWidget::saveSettings( void )
{
	configManager::inst()->setValue( "midialsaseq", "device",
							m_device->text() );
}


#include "midi_alsa_seq.moc"


#endif

