#include "MessageDirector.h"
#include <algorithm>
#include <functional>
#include <boost/icl/interval_bounds.hpp>
using namespace std;

#include "config/ConfigVariable.h"
#include "config/constraints.h"
#include "core/global.h"
#include "core/msgtypes.h"
#include "net/TcpAcceptor.h"
#include "messagedirector/MDNetworkParticipant.h"
#include "messagedirector/MDNetworkUpstream.h"

static ConfigGroup md_config("messagedirector");
static ConfigVariable<string> bind_addr("bind", "unspecified", md_config);
static ConfigVariable<string> connect_addr("connect", "unspecified", md_config);
static ValidAddressConstraint valid_bind_addr(bind_addr);
static ValidAddressConstraint valid_connect_addr(connect_addr);
static ConfigVariable<bool> threaded_mode("threaded", true, md_config);

static ConfigGroup daemon_config("daemon");
static ConfigVariable<string> daemon_name("name", "<unnamed>", daemon_config);
static ConfigVariable<string> daemon_url("url", "", daemon_config);

MessageDirector MessageDirector::singleton;


MessageDirector::MessageDirector() : m_main_thread(this_thread::get_id()) {}
MessageDirector::~MessageDirector()
{
    shutdown_threading();

    while(!m_participants.empty()) {
        m_terminated_participants.push_back(m_participants.front());
        m_participants.pop_front();
    }

    process_terminates();
}

void MessageDirector::init_network()
{
    if(!m_initialized) {
        // Bind to port and listen for downstream servers
        if(bind_addr.get_val() != "unspecified") {
            m_log.info() << "Opening listening socket...\n";

            TcpAcceptorCallback callback = bind(&MessageDirector::handle_connection,
                                           this, placeholders::_1);
            m_net_acceptor = new TcpAcceptor(io_service, callback);
            boost::system::error_code ec;
            ec = m_net_acceptor->bind(bind_addr.get_val(), 7199);
            if(ec.value() != 0) {
                m_log.fatal() << "Could not bind listening port: "
                              << bind_addr.get_val() << ".\n";
                m_log.fatal() << "Error code: " << ec.value()
                              << "(" << ec.category().message(ec.value()) << ").\n";
                exit(1);
            }
            m_net_acceptor->start();
        }

        // Connect to upstream server and start handling received messages
        if(connect_addr.get_val() != "unspecified") {
            m_log.info() << "Connecting upstream...\n";

            MDNetworkUpstream *upstream = new MDNetworkUpstream(this);

            boost::system::error_code ec;
            upstream->connect(connect_addr.get_val());
            if(ec.value() != 0) {
                m_log.fatal() << "Could not connect to remote MD at IP: "
                              << connect_addr.get_val() << ".\n";
                m_log.fatal() << "Error code: " << ec.value()
                              << "(" << ec.category().message(ec.value()) << ").\n";
                exit(1);
            }

            m_upstream = upstream;
        }

        if(threaded_mode.get_val()) {
            m_thread = new thread(bind(&MessageDirector::routing_thread, this));
        }

        m_initialized = true;
    }
}

void MessageDirector::shutdown_threading()
{
    if(!m_thread) {
        return;
    }

    // Signal routing thread to shut down:
    {
        lock_guard<mutex> lock(m_messages_lock);
        m_shutdown = true;
        m_cv.notify_one();
    }

    // Wait for it to do so:
    m_thread->join();
    delete m_thread;
}

void MessageDirector::route_datagram(MDParticipantInterface *p, DatagramHandle dg)
{
    if(m_thread) {
        // Threaded mode! First, we have to get the lock to our queue:
        lock_guard<mutex> lock(m_messages_lock);

        // Now, we put the message into our queue and ring the bell:
        m_messages.push(make_pair(p, dg));
        m_cv.notify_one();
    } else if(this_thread::get_id() != m_main_thread) {
        // We aren't working in threaded mode, but we aren't in the main thread
        // either. For safety, we should post this down to the main thread.
        io_service.post(boost::bind(&MessageDirector::process_datagram, this, p, dg));
    } else {
        // Main thread; we can just process it here.
        process_datagram(p, dg);
    }
}

// This function runs in a thread; it loops until it's told to shut down:
void MessageDirector::routing_thread()
{
    unique_lock<mutex> lock(m_messages_lock);

    while(true) {
        // Wait for something interesting to handle...
        while(m_messages.empty() && !m_shutdown) {
            m_cv.wait(lock);
        }

        if(m_shutdown) {
            // Ehh, we've been told to shut off:
            return;
        }

        // Get and process the message:
        auto msg = m_messages.front();
        m_messages.pop();

        lock.unlock();
        process_datagram(msg.first, msg.second);
        lock.lock();
    }
}

void MessageDirector::process_datagram(MDParticipantInterface *p, DatagramHandle dg)
{
    m_log.trace() << "Processing datagram...\n";

    list<channel_t> channels;
    DatagramIterator dgi(dg);
    try {
        // Unpack channels to send messages to
        uint8_t channel_count = dgi.read_uint8();
        auto receive_log = m_log.trace();
        receive_log << "Receivers: ";
        for(uint8_t i = 0; i < channel_count; ++i) {
            channel_t channel = dgi.read_channel();
            receive_log << channel << ", ";
            channels.push_back(channel);
        }
        receive_log << "\n";
    } catch(DatagramIteratorEOF &) {
        // Log error with receivers output
        if(p) {
            m_log.error() << "Detected truncated datagram reading header from '"
                          << p->m_name << "'.\n";
        } else {
            m_log.error() << "Detected truncated datagram reading header from "
                          "unknown participant.\n";
        }
        return;
    }

    // Find the participants that need to receive the message
    set<ChannelSubscriber*> receiving_participants;
    lookup_channels(channels, receiving_participants);
    if(p) { receiving_participants.erase(p); }

    // Send the datagram to each participant
    for(auto it = receiving_participants.begin(); it != receiving_participants.end(); ++it) {
        auto participant = static_cast<MDParticipantInterface*>(*it);
        DatagramIterator msg_dgi(dg, dgi.tell());

        try { participant->handle_datagram(dg, msg_dgi); }
        catch(DatagramIteratorEOF &) {
            // Log error with receivers output
            m_log.error() << "Detected truncated datagram in handle_datagram for '"
                          << participant->m_name << "' from participant '" << p->m_name << "'.\n";
            return;
        }
    }

    // Send message upstream, if necessary
    if(p && m_upstream) {
        m_upstream->handle_datagram(dg);
        m_log.trace() << "...routing upstream.\n";
    } else if(!p) {
        // If there is no participant, then it came from the upstream
        m_log.trace() << "...not routing upstream: It came from there.\n";
    } else {
        // Otherwise this is the root MessageDirector.
        m_log.trace() << "...not routing upstream: There is none.\n";
    }

    // N.B. Participants may reach end-of-life after receiving a datagram, or may
    // be terminated in another thread (for example if a network socket closes);
    // either way, process any received terminates after processing a datagram.
    process_terminates();
}


void MessageDirector::process_terminates()
{
    lock_guard<mutex> lock(m_terminated_lock);
    while(!m_terminated_participants.empty()) {
        delete m_terminated_participants.front();
        m_terminated_participants.pop_front();
    }
}

void MessageDirector::on_add_channel(channel_t c)
{
    if(m_upstream) { m_upstream->subscribe_channel(c); }
}

void MessageDirector::on_remove_channel(channel_t c)
{
    if(m_upstream) { m_upstream->unsubscribe_channel(c); }
}

void MessageDirector::on_add_range(channel_t lo, channel_t hi)
{
    if(m_upstream) { m_upstream->subscribe_range(lo, hi); }
}

void MessageDirector::on_remove_range(channel_t lo, channel_t hi)
{
    if(m_upstream) { m_upstream->unsubscribe_range(lo, hi); }
}

void MessageDirector::handle_connection(tcp::socket *socket)
{
    tcp::endpoint remote;
    remote = socket->remote_endpoint();
    m_log.info() << "Got an incoming connection from "
                 << remote.address() << ":" << remote.port() << ".\n";
    new MDNetworkParticipant(socket); // It deletes itself when connection is lost
}

void MessageDirector::add_participant(MDParticipantInterface* p)
{
    lock_guard<mutex> lock(m_participants_lock);
    m_participants.push_back(p);
}

void MessageDirector::remove_participant(MDParticipantInterface* p)
{
    // Unsubscribe the participant from any remaining channels
    unsubscribe_all(p);

    // Stop tracking participant
    {
        lock_guard<mutex> lock(m_participants_lock);
        m_participants.remove(p);
    }

    // Send out any post-remove messages the participant may have added.
    // N.B. this is done last, because we don't want to send messages
    // through the Director while a participant is being removed, as
    // certain data structures may not have their invariants satisfied
    // during that time.
    p->post_remove();

    // Mark the participant for deletion
    {
        lock_guard<mutex> lock(m_terminated_lock);
        m_terminated_participants.push_back(p);
    }
}

void MessageDirector::preroute_post_remove(channel_t sender, DatagramHandle post_remove)
{
    if(m_upstream != nullptr) {
        DatagramPtr dg = Datagram::create(CONTROL_ADD_POST_REMOVE);
        dg->add_channel(sender);
        dg->add_blob(post_remove);
        m_upstream->handle_datagram(dg);
    }
}

void MessageDirector::recall_post_removes(channel_t sender)
{
    if(m_upstream != nullptr) {
        DatagramPtr dg = Datagram::create(CONTROL_CLEAR_POST_REMOVES);
        dg->add_channel(sender);
        m_upstream->handle_datagram(dg);
    }
}

void MessageDirector::receive_datagram(DatagramHandle dg)
{
    route_datagram(NULL, dg);
}

void MessageDirector::receive_disconnect()
{
    m_log.fatal() << "Lost connection to upstream md.\n";
    exit(1);
}
