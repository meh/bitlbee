/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Jabber module - I/O stuff (plain, SSL), queues, etc                      *
*                                                                           *
*  Copyright 2006 Wilmer van der Gaast <wilmer@gaast.net>                   *
*                                                                           *
*  This program is free software; you can redistribute it and/or modify     *
*  it under the terms of the GNU General Public License as published by     *
*  the Free Software Foundation; either version 2 of the License, or        *
*  (at your option) any later version.                                      *
*                                                                           *
*  This program is distributed in the hope that it will be useful,          *
*  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
*  GNU General Public License for more details.                             *
*                                                                           *
*  You should have received a copy of the GNU General Public License along  *
*  with this program; if not, write to the Free Software Foundation, Inc.,  *
*  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.              *
*                                                                           *
\***************************************************************************/

#include "jabber.h"

static gboolean jabber_write_callback( gpointer data, gint fd, b_input_condition cond );

int jabber_write_packet( struct gaim_connection *gc, struct xt_node *node )
{
	char *buf;
	int st;
	
	buf = xt_to_string( node );
	st = jabber_write( gc, buf, strlen( buf ) );
	g_free( buf );
	
	return st;
}

int jabber_write( struct gaim_connection *gc, char *buf, int len )
{
	struct jabber_data *jd = gc->proto_data;
	
	if( jd->tx_len == 0 )
	{
		/* If the queue is empty, allocate a new buffer. */
		jd->tx_len = len;
		jd->txq = g_memdup( buf, len );
		
		/* Try if we can write it immediately so we don't have to do
		   it via the event handler. If not, add the handler. (In
		   most cases it probably won't be necessary.) */
		if( jabber_write_callback( gc, jd->fd, GAIM_INPUT_WRITE ) )
			jd->w_inpa = b_input_add( jd->fd, GAIM_INPUT_WRITE, jabber_write_callback, gc );
	}
	else
	{
		/* Just add it to the buffer if it's already filled. The
		   event handler is already set. */
		jd->txq = g_renew( char, jd->txq, jd->tx_len + len );
		memcpy( jd->txq + jd->tx_len, buf, len );
		jd->tx_len += len;
	}
	
	/* FIXME: write_callback could've generated a real error! */
	return 1;
}

static gboolean jabber_write_callback( gpointer data, gint fd, b_input_condition cond )
{
	struct gaim_connection *gc = data;
	struct jabber_data *jd = gc->proto_data;
	int st;
	
	if( jd->fd == -1 )
		return FALSE;
	
	st = write( jd->fd, jd->txq, jd->tx_len );
	
	if( st == jd->tx_len )
	{
		/* We wrote everything, clear the buffer. */
		g_free( jd->txq );
		jd->txq = NULL;
		jd->tx_len = 0;
		
		return FALSE;
	}
	else if( st == 0 || ( st < 0 && !sockerr_again() ) )
	{
		/* Set fd to -1 to make sure we won't write to it anymore. */
		closesocket( jd->fd );	/* Shouldn't be necessary after errors? */
		jd->fd = -1;
		
		hide_login_progress_error( gc, "Short write() to server" );
		signoff( gc );
		return FALSE;
	}
	else if( st > 0 )
	{
		char *s;
		
		s = g_memdup( jd->txq + st, jd->tx_len - st );
		jd->tx_len -= st;
		g_free( jd->txq );
		jd->txq = s;
		
		return TRUE;
	}
	else
	{
		/* Just in case we had EINPROGRESS/EAGAIN: */
		
		return TRUE;
	}
}

static gboolean jabber_read_callback( gpointer data, gint fd, b_input_condition cond )
{
	struct gaim_connection *gc = data;
	struct jabber_data *jd = gc->proto_data;
	char buf[512];
	int st;
	
	if( jd->fd == -1 )
		return FALSE;
	
	st = read( fd, buf, sizeof( buf ) );
	
	if( st > 0 )
	{
		/* Parse. */
		if( !xt_feed( jd->xt, buf, st ) )
		{
			hide_login_progress_error( gc, "XML stream error" );
			signoff( gc );
			return FALSE;
		}
		
		/* Execute all handlers. */
		if( !xt_handle( jd->xt, NULL ) )
		{
			/* Don't do anything, the handlers should have
			   aborted the connection already... Or not? FIXME */
			return FALSE;
		}
		
		if( jd->flags & JFLAG_STREAM_RESTART )
		{
			jd->flags &= ~JFLAG_STREAM_RESTART;
			jabber_start_stream( gc );
		}
		
		/* Garbage collection. */
		xt_cleanup( jd->xt, NULL );
		
		/* This is a bit hackish, unfortunately. Although xmltree
		   has nifty event handler stuff, it only calls handlers
		   when nodes are complete. Since the server should only
		   send an opening <stream:stream> tag, we have to check
		   this by hand. :-( */
		if( !( jd->flags & JFLAG_STREAM_STARTED ) && jd->xt && jd->xt->root )
		{
			if( g_strcasecmp( jd->xt->root->name, "stream:stream" ) == 0 )
			{
				jd->flags |= JFLAG_STREAM_STARTED;
				
				/* If there's no version attribute, assume
				   this is an old server that can't do SASL
				   authentication. */
				if( !sasl_supported( gc ) )
					return jabber_start_iq_auth( gc );
			}
			else
			{
				hide_login_progress_error( gc, "XML stream error" );
				signoff( gc );
				return FALSE;
			}
		}
	}
	else if( st == 0 || ( st < 0 && !sockerr_again() ) )
	{
		closesocket( jd->fd );
		jd->fd = -1;
		
		hide_login_progress_error( gc, "Error while reading from server" );
		signoff( gc );
		return FALSE;
	}
	
	/* EAGAIN/etc or a successful read. */
	return TRUE;
}

gboolean jabber_connected_plain( gpointer data, gint source, b_input_condition cond )
{
	struct gaim_connection *gc = data;
	
	if( source == -1 )
	{
		hide_login_progress( gc, "Could not connect to server" );
		signoff( gc );
		return FALSE;
	}
	
	set_login_progress( gc, 1, "Connected to server, logging in" );
	
	return jabber_start_stream( gc );
}

static xt_status jabber_end_of_stream( struct xt_node *node, gpointer data )
{
	return XT_ABORT;
}

static xt_status jabber_pkt_features( struct xt_node *node, gpointer data )
{
	struct gaim_connection *gc = data;
	struct jabber_data *jd = gc->proto_data;
	struct xt_node *c;
	
	c = xt_find_node( node->children, "starttls" );
	if( c )
	{
		/*
		jd->flags |= JFLAG_SUPPORTS_TLS;
		if( xt_find_node( c->children, "required" ) )
			jd->flags |= JFLAG_REQUIRES_TLS;
		*/
	}
	
	/* This flag is already set if we authenticated via SASL, so now
	   we can resume the session in the new stream. */
	if( jd->flags & JFLAG_AUTHENTICATED )
	{
		if( !jabber_get_roster( gc ) )
			return XT_ABORT;
	}
	
	return XT_HANDLED;
}

static xt_status jabber_pkt_misc( struct xt_node *node, gpointer data )
{
	printf( "Received unknown packet:\n" );
	xt_print( node );
	
	return XT_HANDLED;
}

static const struct xt_handler_entry jabber_handlers[] = {
	{ "stream:stream",      "<root>",               jabber_end_of_stream },
	{ "message",            "stream:stream",        jabber_pkt_message },
	{ "presence",           "stream:stream",        jabber_pkt_presence },
	{ "iq",                 "stream:stream",        jabber_pkt_iq },
	{ "stream:features",    "stream:stream",        jabber_pkt_features },
	{ "mechanisms",         "stream:features",      sasl_pkt_mechanisms },
	{ "challenge",          "stream:stream",        sasl_pkt_challenge },
	{ "success",            "stream:stream",        sasl_pkt_result },
	{ "failure",            "stream:stream",        sasl_pkt_result },
	{ NULL,                 "stream:stream",        jabber_pkt_misc },
	{ NULL,                 NULL,                   NULL }
};

gboolean jabber_start_stream( struct gaim_connection *gc )
{
	struct jabber_data *jd = gc->proto_data;
	int st;
	char *greet;
	
	/* We'll start our stream now, so prepare everything to receive one
	   from the server too. */
	xt_free( jd->xt );	/* In case we're RE-starting. */
	jd->xt = xt_new( gc );
	jd->xt->handlers = (struct xt_handler_entry*) jabber_handlers;
	
	if( jd->r_inpa <= 0 )
		jd->r_inpa = b_input_add( jd->fd, GAIM_INPUT_READ, jabber_read_callback, gc );
	
	greet = g_strdup_printf( "<?xml version='1.0' ?>"
	                         "<stream:stream to=\"%s\" xmlns=\"jabber:client\" "
	                          "xmlns:stream=\"http://etherx.jabber.org/streams\" version=\"1.0\">", jd->server );
	
	st = jabber_write( gc, greet, strlen( greet ) );
	
	g_free( greet );
	
	return st;
}

void jabber_end_stream( struct gaim_connection *gc )
{
	struct jabber_data *jd = gc->proto_data;
	
	/* Let's only do this if the queue is currently empty, otherwise it'd
	   take too long anyway. */
	if( jd->tx_len == 0 )
	{
		char eos[] = "</stream:stream>";
		struct xt_node *node;
		int st = 1;
		
		if( gc->flags & OPT_LOGGED_IN )
		{
			node = jabber_make_packet( "presence", "unavailable", NULL, NULL );
			st = jabber_write_packet( gc, node );
			xt_free_node( node );
		}
		
		if( st )
			jabber_write( gc, eos, strlen( eos ) );
	}
}
