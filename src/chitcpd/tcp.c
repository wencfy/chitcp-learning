/*
 *  chiTCP - A simple, testable TCP stack
 *
 *  Implementation of the TCP protocol.
 *
 *  chiTCP follows a state machine approach to implementing TCP.
 *  This means that there is a handler function for each of
 *  the TCP states (CLOSED, LISTEN, SYN_RCVD, etc.). If an
 *  event (e.g., a packet arrives) while the connection is
 *  in a specific state (e.g., ESTABLISHED), then the handler
 *  function for that state is called, along with information
 *  about the event that just happened.
 *
 *  Each handler function has the following prototype:
 *
 *  int f(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event);
 *
 *  si is a pointer to the chiTCP server info. The functions in
 *       this file will not have to access the data in the server info,
 *       but this pointer is needed to call other functions.
 *
 *  entry is a pointer to the socket entry for the connection that
 *          is being handled. The socket entry contains the actual TCP
 *          data (variables, buffers, etc.), which can be extracted
 *          like this:
 *
 *            tcp_data_t *tcp_data = &entry->socket_state.active.tcp_data;
 *
 *          Other than that, no other fields in "entry" should be read
 *          or modified.
 *
 *  event is the event that has caused the TCP thread to wake up. The
 *          list of possible events corresponds roughly to the ones
 *          specified in http://tools.ietf.org/html/rfc793#section-3.9.
 *          They are:
 *
 *            APPLICATION_CONNECT: Application has called socket_connect()
 *            and a three-way handshake must be initiated.
 *
 *            APPLICATION_SEND: Application has called socket_send() and
 *            there is unsent data in the send buffer.
 *
 *            APPLICATION_RECEIVE: Application has called socket_recv() and
 *            any received-and-acked data in the recv buffer will be
 *            collected by the application (up to the maximum specified
 *            when calling socket_recv).
 *
 *            APPLICATION_CLOSE: Application has called socket_close() and
 *            a connection tear-down should be initiated.
 *
 *            PACKET_ARRIVAL: A packet has arrived through the network and
 *            needs to be processed (RFC 793 calls this "SEGMENT ARRIVES")
 *
 *            TIMEOUT: A timeout (e.g., a retransmission timeout) has
 *            happened.
 *
 */

/*
 *  Copyright (c) 2013-2014, The University of Chicago
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or withsend
 *  modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  - Neither the name of The University of Chicago nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software withsend specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY send OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "chitcp/log.h"
#include "chitcp/utils.h"
#include "chitcp/buffer.h"
#include "chitcp/chitcpd.h"
#include "serverinfo.h"
#include "connection.h"
#include "tcp.h"
#include <stdlib.h>
#include <string.h>

void handle_PACKET_ARRIVAL(serverinfo_t *, chisocketentry_t *, tcp_state_t);

tcp_packet_t *ACK_PACKET(chisocketentry_t *, tcp_data_t *);
tcp_packet_t *SYN_ACK_PACKET(chisocketentry_t *, tcp_data_t *);

void tcp_data_init(serverinfo_t *si, chisocketentry_t *entry)
{
    tcp_data_t *tcp_data = &entry->socket_state.active.tcp_data;

    tcp_data->pending_packets = NULL;
    pthread_mutex_init(&tcp_data->lock_pending_packets, NULL);
    pthread_cond_init(&tcp_data->cv_pending_packets, NULL);

    /* Initialization of additional tcp_data_t fields,
     * and creation of retransmission thread, goes here */
}

void tcp_data_free(serverinfo_t *si, chisocketentry_t *entry)
{
    tcp_data_t *tcp_data = &entry->socket_state.active.tcp_data;

    circular_buffer_free(&tcp_data->send);
    circular_buffer_free(&tcp_data->recv);
    chitcp_packet_list_destroy(&tcp_data->pending_packets);
    pthread_mutex_destroy(&tcp_data->lock_pending_packets);
    pthread_cond_destroy(&tcp_data->cv_pending_packets);

    /* Cleanup of additional tcp_data_t fields goes here */
}


int chitcpd_tcp_state_handle_CLOSED(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    tcp_data_t *data = &entry->socket_state.active.tcp_data;

    if (event == APPLICATION_CONNECT)
    {
        /* Your code goes here */
        data->ISS     = rand() % 1000 + 1;
        data->SND_UNA = data->ISS;
        data->SND_NXT = data->ISS + 1;
        
        data->RCV_WND = circular_buffer_available(&data->recv);
        
        tcp_packet_t *packet = malloc(sizeof(tcp_packet_t));
        chitcpd_tcp_packet_create(entry, packet, NULL, 0);
        tcphdr_t *SYN = TCP_PACKET_HEADER(packet);

        SYN->seq     = chitcp_htonl(data->ISS);
        // SYN->ack_seq = chitcp_htonl(data->ISS + 1);
        SYN->syn     = 1;
        SYN->win     = chitcp_htons(data->RCV_WND);
        

        chilog_tcp(CRITICAL, packet, LOG_OUTBOUND);
        chitcpd_send_tcp_packet(si, entry, packet);

        chitcpd_update_tcp_state(si, entry, SYN_SENT);
        chitcp_tcp_packet_free(packet);
    }
    else if (event == CLEANUP)
    {
        /* Any additional cleanup goes here */
    }
    else
        chilog(WARNING, "In CLOSED state, received unexpected event.");

    return CHITCP_OK;
}

int chitcpd_tcp_state_handle_LISTEN(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */
        handle_PACKET_ARRIVAL(si, entry, LISTEN);
    }
    else
        chilog(WARNING, "In LISTEN state, received unexpected event.");

    return CHITCP_OK;
}

int chitcpd_tcp_state_handle_SYN_RCVD(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        handle_PACKET_ARRIVAL(si, entry, SYN_RCVD);
    }
    else if (event == TIMEOUT_RTX)
    {
        /* Your code goes here */
    }
    else
        chilog(WARNING, "In SYN_RCVD state, received unexpected event.");

    return CHITCP_OK;
}

int chitcpd_tcp_state_handle_SYN_SENT(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        handle_PACKET_ARRIVAL(si, entry, SYN_SENT);
    }
    else if (event == TIMEOUT_RTX)
    {
        /* Your code goes here */
    }
    else
        chilog(WARNING, "In SYN_SENT state, received unexpected event.");

    return CHITCP_OK;
}

int chitcpd_tcp_state_handle_ESTABLISHED(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == APPLICATION_SEND)
    {
        /* Your code goes here */
    }
    else if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */

    }
    else if (event == APPLICATION_RECEIVE)
    {
        /* Your code goes here */
    }
    else if (event == APPLICATION_CLOSE)
    {
        /* Your code goes here */
    }
    else if (event == TIMEOUT_RTX)
    {
      /* Your code goes here */
    }
    else if (event == TIMEOUT_PST)
    {
        /* Your code goes here */
    }
    else
        chilog(WARNING, "In ESTABLISHED state, received unexpected event (%i).", event);

    return CHITCP_OK;
}

int chitcpd_tcp_state_handle_FIN_WAIT_1(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */
    }
    else if (event == APPLICATION_RECEIVE)
    {
        /* Your code goes here */
    }
    else if (event == TIMEOUT_RTX)
    {
      /* Your code goes here */
    }
    else if (event == TIMEOUT_PST)
    {
        /* Your code goes here */
    }
    else
       chilog(WARNING, "In FIN_WAIT_1 state, received unexpected event (%i).", event);

    return CHITCP_OK;
}


int chitcpd_tcp_state_handle_FIN_WAIT_2(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */
    }
    else if (event == APPLICATION_RECEIVE)
    {
        /* Your code goes here */
    }
    else if (event == TIMEOUT_RTX)
    {
      /* Your code goes here */
    }
    else
        chilog(WARNING, "In FIN_WAIT_2 state, received unexpected event (%i).", event);

    return CHITCP_OK;
}


int chitcpd_tcp_state_handle_CLOSE_WAIT(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == APPLICATION_CLOSE)
    {
        /* Your code goes here */
    }
    else if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */
    }
    else if (event == TIMEOUT_RTX)
    {
      /* Your code goes here */
    }
    else if (event == TIMEOUT_PST)
    {
        /* Your code goes here */
    }
    else
       chilog(WARNING, "In CLOSE_WAIT state, received unexpected event (%i).", event);


    return CHITCP_OK;
}


int chitcpd_tcp_state_handle_CLOSING(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */
    }
    else if (event == TIMEOUT_RTX)
    {
      /* Your code goes here */
    }
    else if (event == TIMEOUT_PST)
    {
        /* Your code goes here */
    }
    else
       chilog(WARNING, "In CLOSING state, received unexpected event (%i).", event);

    return CHITCP_OK;
}


int chitcpd_tcp_state_handle_TIME_WAIT(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    chilog(WARNING, "Running handler for TIME_WAIT. This should not happen.");

    return CHITCP_OK;
}


int chitcpd_tcp_state_handle_LAST_ACK(serverinfo_t *si, chisocketentry_t *entry, tcp_event_type_t event)
{
    if (event == PACKET_ARRIVAL)
    {
        /* Your code goes here */
    }
    else if (event == TIMEOUT_RTX)
    {
      /* Your code goes here */
    }
    else if (event == TIMEOUT_PST)
    {
        /* Your code goes here */
    }
    else
       chilog(WARNING, "In LAST_ACK state, received unexpected event (%i).", event);

    return CHITCP_OK;
}

/*                                                           */
/*     Any additional functions you need should go here      */
/*                                                           */
void handle_PACKET_ARRIVAL(serverinfo_t *si, chisocketentry_t *entry, tcp_state_t state) {
    tcp_data_t *data = &entry->socket_state.active.tcp_data;

    pthread_mutex_lock(&data->lock_pending_packets);
    tcp_packet_t *packet_rcvd = data->pending_packets->packet;
    chitcp_packet_list_pop_head(&data->pending_packets);
    pthread_mutex_unlock(&data->lock_pending_packets);

    tcphdr_t *header = TCP_PACKET_HEADER(packet_rcvd);

    switch (state) {
        case LISTEN: {
            // RST or ACK: related to RST, do not consider
            // check for SYN
            if (header->syn) {
                data->RCV_NXT = SEG_SEQ(packet_rcvd) + 1;
                data->IRS     = SEG_SEQ(packet_rcvd);
                
                data->ISS     = rand() % 1000 + 1;

                data->RCV_WND = circular_buffer_available(&data->recv);

                tcp_packet_t *syn_ack_packet = SYN_ACK_PACKET(entry, data);

                chilog_tcp(CRITICAL, syn_ack_packet, LOG_OUTBOUND);
                chitcpd_send_tcp_packet(si, entry, syn_ack_packet);

                data->SND_NXT = data->ISS + 1;
                data->SND_UNA = data->ISS;
                data->SND_WND = SEG_WND(packet_rcvd);

                chitcpd_update_tcp_state(si, entry, SYN_RCVD);
                chitcp_tcp_packet_free(syn_ack_packet);
            }
        }
        break;

        case SYN_SENT: {
            if (header->ack) {
                if (
                    SEG_ACK(packet_rcvd) <= data->ISS ||
                    SEG_ACK(packet_rcvd) > data->SND_NXT
                ) {
                    // illegal segment ack code, RST
                    // no neet to implement
                } else if (
                    data->SND_UNA <= SEG_ACK(packet_rcvd) &&
                    SEG_ACK(packet_rcvd) <= data->SND_NXT
                ) {
                    // ACK acceptable

                    if (header->syn) {
                        // ACK is OK && SYN

                        data->RCV_NXT = SEG_SEQ(packet_rcvd) + 1;
                        data->IRS     = SEG_SEQ(packet_rcvd);
                        data->SND_UNA = SEG_ACK(packet_rcvd);
                        data->SND_WND = SEG_WND(packet_rcvd);

                        if (data->SND_UNA > data->ISS) {
                            // our SYN has been ACKed, change the connection 
                            // state to ESTABLISHED, form an ACK segment

                            tcp_packet_t *ack_packet = ACK_PACKET(entry, data);
                            chilog_tcp(CRITICAL, ack_packet, LOG_OUTBOUND);
                            chitcpd_send_tcp_packet(si, entry, ack_packet);

                            chitcpd_update_tcp_state(si, entry, ESTABLISHED);
                            chitcp_tcp_packet_free(ack_packet);
                        } else {
                            // SYN has not been ACKed, retransmit SYN_ACK packet
                            // enter SYN-RECEIVED

                            tcp_packet_t *syn_ack_packet = SYN_ACK_PACKET(entry, data);

                            chilog_tcp(CRITICAL, syn_ack_packet, LOG_OUTBOUND);
                            chitcpd_send_tcp_packet(si, entry, syn_ack_packet);

                            chitcpd_update_tcp_state(si, entry, SYN_RCVD);
                            chitcp_tcp_packet_free(syn_ack_packet);
                        }
                    }
                }
            } else {
                // not ACK

                if (header->syn) {
                    // no ACK && SYN

                    
                }
            }
        }
        break;
        
        case SYN_RCVD: {
            if (header->ack) {
                if (
                    data->SND_UNA <= SEG_ACK(packet_rcvd) &&
                    SEG_ACK(packet_rcvd) <= data->SND_NXT
                ) {
                    data->RCV_NXT = SEG_SEQ(packet_rcvd) + 1;
                    data->SND_UNA = SEG_ACK(packet_rcvd);
                    data->SND_WND = SEG_WND(packet_rcvd);

                    chitcpd_update_tcp_state(si, entry, ESTABLISHED);
                }
            }
        }
        break;
    }

    // free packet_rcvd
    chitcp_tcp_packet_free(packet_rcvd);
}

tcp_packet_t *ACK_PACKET(chisocketentry_t *entry, tcp_data_t *data) {
    tcp_packet_t *packet = malloc(sizeof(tcp_packet_t));
    chitcpd_tcp_packet_create(entry, packet, NULL, 0);
    tcphdr_t *SYN_ACK = TCP_PACKET_HEADER(packet);

    SYN_ACK->ack     = 1;
    SYN_ACK->seq     = htonl(data->SND_NXT);
    SYN_ACK->ack_seq = htonl(data->RCV_NXT);
    SYN_ACK->win     = htons(data->RCV_WND);
    
    return packet;
}

tcp_packet_t *SYN_ACK_PACKET(chisocketentry_t *entry, tcp_data_t *data) {
    tcp_packet_t *packet = malloc(sizeof(tcp_packet_t));
    chitcpd_tcp_packet_create(entry, packet, NULL, 0);
    tcphdr_t *SYN_ACK = TCP_PACKET_HEADER(packet);

    SYN_ACK->syn     = 1;
    SYN_ACK->ack     = 1;
    SYN_ACK->seq     = htonl(data->ISS);
    SYN_ACK->ack_seq = htonl(data->RCV_NXT);
    SYN_ACK->win     = htons(data->RCV_WND);
    
    return packet;
}
