#pragma once
#include "net/NetworkClient.h"
#include "messagedirector/MessageDirector.h"

class MDNetworkParticipant : public MDParticipantInterface, public NetworkClient
{
  public:
    MDNetworkParticipant(tcp::socket *socket);
    void handle_datagram(DatagramHandle dg, DatagramIterator &dgi) override;

  private:
    void receive_datagram(DatagramHandle dg) override;
    void receive_disconnect() override;
};
