#include "IrcServer.hpp"

Channel::Channel(const std::string &channelName)
: name(channelName), hasTopic(false), inviteOnly(false), topicProtected(false),
  hasKey(false), hasLimit(false), limit(0) {}

Channel::~Channel() {}
