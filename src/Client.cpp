#include "IrcServer.hpp"

Client::Client()
: fd(-1), passOk(false), nickSet(false), userSet(false), registered(false), pendingQuit(false) {}

Client::~Client() {}
