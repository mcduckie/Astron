#pragma once
#include <boost/asio.hpp>
#include "net/NetworkClient.h"
#include "messagedirector/MessageDirector.h"

// All MDUpstreams must be thread-safe. This class does not need a lock, however,
// because all of its operations are based on local variables and the functions
// of NetworkClient (which are themselves thread-safe)
class MDNetworkUpstream : public NetworkClient, public MDUpstream
{
  public:
    MDNetworkUpstream(MessageDirector *md);
    MDNetworkUpstream(const MDNetworkUpstream&) = delete;
    MDNetworkUpstream& operator=(const MDNetworkUpstream&) = delete;
    virtual ~MDNetworkUpstream() {}

    boost::system::error_code connect(const std::string &address);

    // Interfaces that MDUpstream needs us to implement:
    void subscribe_channel(channel_t c) override;
    void unsubscribe_channel(channel_t c) override;
    void subscribe_range(channel_t lo, channel_t hi) override;
    void unsubscribe_range(channel_t lo, channel_t hi) override;
    void handle_datagram(DatagramHandle dg) override;

    // Interfaces that NetworkClient needs us to implement:
    void receive_datagram(DatagramHandle dg) override;
    void receive_disconnect() override;

  private:
    MessageDirector *m_message_director;
};
