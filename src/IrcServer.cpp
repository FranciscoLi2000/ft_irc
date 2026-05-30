#include "IrcServer.hpp"

namespace {
	std::string joinTokens(const std::vector<std::string> &args, std::size_t start)
	{
		std::string out;
		for (std::size_t i = start; i < args.size(); ++i)
		{
			if (i > start)
				out += " ";
			out += args[i];
		}
		return out;
	}
}

IrcServer::IrcServer(int port, const std::string &password)
: _port(port), _password(password), _listenFd(-1), _running(true) {}

IrcServer::~IrcServer()
{
	for (std::map<int, Client *>::iterator it = _clients.begin(); it != _clients.end(); ++it)
	{
		if (it->second)
		{
			close(it->second->fd);
			delete it->second;
		}
	}
	for (std::map<std::string, Channel *>::iterator it = _channels.begin(); it != _channels.end(); ++it)
		delete it->second;
	if (_listenFd != -1)
		close(_listenFd);
}

void IrcServer::setupSocket()
{
	_listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (_listenFd == -1)
		throw std::runtime_error("socket");
	int yes = 1;
	if (setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
		throw std::runtime_error("setsockopt");
	int flags = fcntl(_listenFd, F_GETFL, 0);
	if (flags == -1 || fcntl(_listenFd, F_SETFL, flags | O_NONBLOCK) == -1)
		throw std::runtime_error("fcntl");
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(_port);
	if (bind(_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1)
		throw std::runtime_error("bind");
	if (listen(_listenFd, SOMAXCONN) == -1)
		throw std::runtime_error("listen");
}

void IrcServer::run()
{
	setupSocket();
	while (_running)
	{
		std::vector<pollfd> pollfds;
		pollfd serverPfd;
		serverPfd.fd = _listenFd;
		serverPfd.events = POLLIN;
		serverPfd.revents = 0;
		pollfds.push_back(serverPfd);
		for (std::map<int, Client *>::iterator it = _clients.begin(); it != _clients.end(); ++it)
		{
			pollfd pfd;
			pfd.fd = it->first;
			pfd.events = POLLIN;
			if (it->second && !it->second->outBuffer.empty())
				pfd.events |= POLLOUT;
			pfd.revents = 0;
			pollfds.push_back(pfd);
		}
		int ret = poll(&pollfds[0], pollfds.size(), -1);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			throw std::runtime_error("poll");
		}
		if (pollfds[0].revents & POLLIN)
			acceptClient();
		for (std::size_t i = 1; i < pollfds.size(); ++i)
		{
			int fd = pollfds[i].fd;
			if (_clients.find(fd) == _clients.end())
				continue;
			if (pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
			{
				disconnectClient(fd, "Client disconnected");
				continue;
			}
			if (pollfds[i].revents & POLLIN)
				handleClientRead(fd);
			if (_clients.find(fd) != _clients.end() && (pollfds[i].revents & POLLOUT))
				handleClientWrite(fd);
		}
	}
}

void IrcServer::acceptClient()
{
	while (true)
	{
		sockaddr_in addr;
		socklen_t len = sizeof(addr);
		int fd = accept(_listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
		if (fd == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			throw std::runtime_error("accept");
		}
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		{
			close(fd);
			continue;
		}
		Client *client = new Client();
		client->fd = fd;
		_clients[fd] = client;
		queueMessage(*client, prefixForServer() + " NOTICE * :Welcome to ft_irc\r\n");
	}
}

void IrcServer::handleClientRead(int fd)
{
	std::map<int, Client *>::iterator it = _clients.find(fd);
	if (it == _clients.end())
		return;
	Client &client = *it->second;
	char buffer[4096];
	while (true)
	{
		ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
		if (n == 0)
		{
			disconnectClient(fd, "Client disconnected");
			return;
		}
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			disconnectClient(fd, "Client disconnected");
			return;
		}
		client.inBuffer.append(buffer, static_cast<std::size_t>(n));
		std::string::size_type pos;
		while ((pos = client.inBuffer.find("\r\n")) != std::string::npos)
		{
			std::string line = client.inBuffer.substr(0, pos);
			client.inBuffer.erase(0, pos + 2);
			processLine(client, trimCRLF(line));
			if (client.pendingQuit)
				break;
		}
		if (client.pendingQuit)
			break;
	}
	if (client.pendingQuit)
	{
		if (client.outBuffer.empty())
			disconnectClient(fd, client.quitMessage, false);
	}
}

void IrcServer::handleClientWrite(int fd)
{
	std::map<int, Client *>::iterator it = _clients.find(fd);
	if (it == _clients.end())
		return;
	Client *client = it->second;
	if (!flushClient(*client))
	{
		disconnectClient(fd, "Client disconnected");
		return;
	}
	if (_clients.find(fd) != _clients.end() && client->pendingQuit && client->outBuffer.empty())
		disconnectClient(fd, client->quitMessage, false);
}

void IrcServer::disconnectClient(int fd, const std::string &reason, bool notify)
{
	std::map<int, Client *>::iterator it = _clients.find(fd);
	if (it == _clients.end())
		return;
	Client &client = *it->second;
	if (notify)
		removeClientFromAllChannels(client, reason);
	close(fd);
	delete it->second;
	_clients.erase(it);
}

void IrcServer::destroyChannelIfEmpty(std::string name)
{
	Channel *channel = findChannel(name);
	if (channel && channel->members.empty())
	{
		_channels.erase(name);
		delete channel;
	}
}

void IrcServer::processLine(Client &client, const std::string &line)
{
	if (line.empty())
		return;
	std::vector<std::string> args = tokenize(line);
	if (args.empty())
		return;
	handleCommand(client, args);
}

void IrcServer::handleCommand(Client &client, const std::vector<std::string> &args)
{
	std::string cmd = toUpper(args[0]);
	if (cmd == "PASS")
		cmdPass(client, args);
	else if (cmd == "NICK")
		cmdNick(client, args);
	else if (cmd == "USER")
		cmdUser(client, args);
	else if (cmd == "PING")
		cmdPing(client, args);
	else if (cmd == "PONG")
		cmdPong(client, args);
	else if (cmd == "QUIT")
		cmdQuit(client, args);
	else if (!client.registered)
		sendError(client, "451", ":You have not registered");
	else if (cmd == "JOIN")
		cmdJoin(client, args);
	else if (cmd == "PART")
		cmdPart(client, args);
	else if (cmd == "PRIVMSG")
		cmdPrivmsg(client, args, false);
	else if (cmd == "NOTICE")
		cmdPrivmsg(client, args, true);
	else if (cmd == "TOPIC")
		cmdTopic(client, args);
	else if (cmd == "MODE")
		cmdMode(client, args);
	else if (cmd == "KICK")
		cmdKick(client, args);
	else if (cmd == "INVITE")
		cmdInvite(client, args);
	else
		sendError(client, "421", cmd + " :Unknown command");
}

std::vector<std::string> IrcServer::tokenize(const std::string &line) const
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < line.size())
	{
		while (i < line.size() && line[i] == ' ')
			++i;
		if (i >= line.size())
			break;
		if (line[i] == ':')
		{
			out.push_back(line.substr(i + 1));
			break;
		}
		std::size_t j = i;
		while (j < line.size() && line[j] != ' ')
			++j;
		out.push_back(line.substr(i, j - i));
		i = j;
	}
	return out;
}

std::string IrcServer::trimCRLF(const std::string &line)
{
	if (line.size() >= 2 && line[line.size() - 2] == '\r' && line[line.size() - 1] == '\n')
		return line.substr(0, line.size() - 2);
	return line;
}

std::string IrcServer::toUpper(std::string value)
{
	for (std::size_t i = 0; i < value.size(); ++i)
		value[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
	return value;
}

bool IrcServer::isChannelName(const std::string &target)
{
	return !target.empty() && (target[0] == '#' || target[0] == '&');
}

std::string IrcServer::prefixFor(const Client &client)
{
	return prefixForIdentity(client.nickSet ? client.nick : "*", client.userSet ? client.user : "*");
}

std::string IrcServer::prefixForIdentity(const std::string &nick, const std::string &user)
{
	return ":" + nick + "!" + user + "@localhost";
}

std::string IrcServer::prefixForServer()
{
	return ":ft_irc";
}

std::string IrcServer::intToString(int value)
{
	std::ostringstream oss;
	oss << value;
	return oss.str();
}

bool IrcServer::isNumber(const std::string &value)
{
	if (value.empty())
		return false;
	for (std::size_t i = 0; i < value.size(); ++i)
		if (!std::isdigit(static_cast<unsigned char>(value[i])))
			return false;
	return true;
}

void IrcServer::queueMessage(Client &client, const std::string &message)
{
	client.outBuffer.append(message);
}

bool IrcServer::flushClient(Client &client)
{
	while (!client.outBuffer.empty())
	{
		ssize_t n = send(client.fd, client.outBuffer.data(), client.outBuffer.size(), 0);
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return true;
			return false;
		}
		client.outBuffer.erase(0, static_cast<std::size_t>(n));
	}
	return true;
}

void IrcServer::sendNumeric(Client &client, const std::string &code, const std::string &message)
{
	queueMessage(client, prefixForServer() + " " + code + " " + (client.nickSet ? client.nick : "*") + " " + message + "\r\n");
}

void IrcServer::sendError(Client &client, const std::string &code, const std::string &message)
{
	sendNumeric(client, code, message);
}

void IrcServer::sendWelcome(Client &client)
{
	sendNumeric(client, "001", ":Welcome to the Internet Relay Network " + client.nick);
	sendNumeric(client, "002", ":Your host is ft_irc");
	sendNumeric(client, "003", ":This server was created just now");
	sendNumeric(client, "004", ":ft_irc 1.0 oiwsz biklmnop");
}

Client *IrcServer::findClientByNick(const std::string &nick) const
{
	for (std::map<int, Client *>::const_iterator it = _clients.begin(); it != _clients.end(); ++it)
		if (it->second && it->second->nickSet && it->second->nick == nick)
			return it->second;
	return NULL;
}

Channel *IrcServer::findChannel(const std::string &name) const
{
	std::map<std::string, Channel *>::const_iterator it = _channels.find(name);
	if (it == _channels.end())
		return NULL;
	return it->second;
}

Channel &IrcServer::getOrCreateChannel(const std::string &name)
{
	Channel *channel = findChannel(name);
	if (!channel)
	{
		channel = new Channel(name);
		_channels[name] = channel;
	}
	return *channel;
}

void IrcServer::removeClientFromAllChannels(Client &client, const std::string &quitMsg)
{
	std::vector<std::string> names(client.joinedChannels.begin(), client.joinedChannels.end());
	for (std::size_t i = 0; i < names.size(); ++i)
	{
		Channel *channel = findChannel(names[i]);
		if (!channel)
			continue;
		channel->members.erase(&client);
		channel->operators.erase(&client);
		channel->invited.erase(&client);
		broadcastToChannel(*channel, prefixFor(client) + " QUIT :" + quitMsg + "\r\n", &client);
		destroyChannelIfEmpty(channel->name);
	}
	client.joinedChannels.clear();
}

void IrcServer::cmdPass(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2)
	{
		sendError(client, "461", "PASS :Not enough parameters");
		return;
	}
	if (client.passOk)
	{
		sendError(client, "462", ":You may not reregister");
		return;
	}
	client.passOk = (args[1] == _password);
	if (!client.passOk)
		sendError(client, "464", ":Password incorrect");
}

void IrcServer::cmdNick(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2 || args[1].empty())
	{
		sendError(client, "431", ":No nickname given");
		return;
	}
	bool isNickTaken = (findClientByNick(args[1]) && (!client.nickSet || client.nick != args[1]));
	if (isNickTaken)
	{
		sendError(client, "433", args[1] + " :Nickname is already in use");
		return;
	}
	std::string oldNick = client.nick;
	client.nick = args[1];
	client.nickSet = true;
	if (!client.registered && client.passOk && client.nickSet && client.userSet)
	{
		client.registered = true;
		sendWelcome(client);
	}
	if (oldNick.empty() || oldNick == client.nick)
		return;
	std::string msg = prefixForIdentity(oldNick, client.userSet ? client.user : "*") + " NICK :" + args[1] + "\r\n";
	for (std::set<std::string>::iterator it = client.joinedChannels.begin(); it != client.joinedChannels.end(); ++it)
	{
		Channel *channel = findChannel(*it);
		if (channel)
			broadcastToChannel(*channel, msg, NULL);
	}
}

void IrcServer::cmdUser(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 5)
	{
		sendError(client, "461", "USER :Not enough parameters");
		return;
	}
	if (client.userSet)
	{
		sendError(client, "462", ":You may not reregister");
		return;
	}
	client.user = args[1];
	client.realname = args[4];
	client.userSet = true;
	if (!client.registered && client.passOk && client.nickSet && client.userSet)
	{
		client.registered = true;
		sendWelcome(client);
	}
}

void IrcServer::cmdPing(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2)
	{
		sendError(client, "409", ":No origin specified");
		return;
	}
	queueMessage(client, prefixForServer() + " PONG " + prefixForServer() + " :" + args[1] + "\r\n");
}

void IrcServer::cmdPong(Client &, const std::vector<std::string> &)
{
}

void IrcServer::cmdQuit(Client &client, const std::vector<std::string> &args)
{
	std::string msg = args.size() > 1 ? joinTokens(args, 1) : "Client Quit";
	removeClientFromAllChannels(client, msg);
	client.quitMessage = msg;
	client.pendingQuit = true;
	queueMessage(client, prefixFor(client) + " QUIT :" + msg + "\r\n");
}

void IrcServer::joinChannel(Client &client, Channel &channel, const std::string &key)
{
	if (channel.hasLimit && channel.members.size() >= channel.limit)
	{
		sendError(client, "471", channel.name + " :Cannot join channel (+l)");
		return;
	}
	if (channel.inviteOnly && channel.invited.find(&client) == channel.invited.end())
	{
		sendError(client, "473", channel.name + " :Cannot join channel (+i)");
		return;
	}
	if (channel.hasKey && channel.key != key)
	{
		sendError(client, "475", channel.name + " :Cannot join channel (+k)");
		return;
	}
	if (channel.members.find(&client) != channel.members.end())
		return;
	channel.members.insert(&client);
	client.joinedChannels.insert(channel.name);
	if (channel.members.size() == 1)
		channel.operators.insert(&client);
	broadcastToChannel(channel, prefixFor(client) + " JOIN :" + channel.name + "\r\n");
	sendChannelTopic(client, channel);
	broadcastMembershipNames(channel, client);
	channel.invited.erase(&client);
}

void IrcServer::cmdJoin(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2)
	{
		sendError(client, "461", "JOIN :Not enough parameters");
		return;
	}
	std::vector<std::string> keys;
	if (args.size() >= 3)
	{
		std::stringstream keyStream(args[2]);
		std::string key;
		while (std::getline(keyStream, key, ','))
			keys.push_back(key);
	}
	std::stringstream ss(args[1]);
	std::string channelName;
	std::size_t index = 0;
	while (std::getline(ss, channelName, ','))
	{
		if (!isChannelName(channelName))
		{
			sendError(client, "403", channelName + " :No such channel");
			continue;
		}
		Channel &channel = getOrCreateChannel(channelName);
		const std::string key = index < keys.size() ? keys[index] : "";
		joinChannel(client, channel, key);
		++index;
	}
}

void IrcServer::partChannel(Client &client, Channel &channel, const std::string &message)
{
	if (channel.members.find(&client) == channel.members.end())
	{
		sendError(client, "442", channel.name + " :You're not on that channel");
		return;
	}
	if (message.empty())
		broadcastToChannel(channel, prefixFor(client) + " PART " + channel.name + "\r\n");
	else
		broadcastToChannel(channel, prefixFor(client) + " PART " + channel.name + " :" + message + "\r\n");
	channel.members.erase(&client);
	channel.operators.erase(&client);
	channel.invited.erase(&client);
	client.joinedChannels.erase(channel.name);
	destroyChannelIfEmpty(channel.name);
}

void IrcServer::cmdPart(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2)
	{
		sendError(client, "461", "PART :Not enough parameters");
		return;
	}
	std::string msg = args.size() >= 3 ? joinTokens(args, 2) : "";
	std::stringstream ss(args[1]);
	std::string channelName;
	while (std::getline(ss, channelName, ','))
	{
		Channel *channel = findChannel(channelName);
		if (!channel)
		{
			sendError(client, "403", channelName + " :No such channel");
			continue;
		}
		partChannel(client, *channel, msg);
	}
}

void IrcServer::cmdPrivmsg(Client &client, const std::vector<std::string> &args, bool notice)
{
	if (args.size() < 3)
	{
		if (!notice)
			sendError(client, "461", "PRIVMSG :Not enough parameters");
		return;
	}
	std::string target = args[1];
	std::string message = args[2];
	if (message.empty())
	{
		if (!notice)
			sendError(client, "412", ":No text to send");
		return;
	}
	if (isChannelName(target))
	{
		Channel *channel = findChannel(target);
		if (!channel)
		{
			if (!notice)
				sendError(client, "403", target + " :No such channel");
			return;
		}
		if (channel->members.find(&client) == channel->members.end())
		{
			if (!notice)
				sendError(client, "404", target + " :Cannot send to channel");
			return;
		}
		broadcastToChannel(*channel, prefixFor(client) + " " + (notice ? "NOTICE" : "PRIVMSG") + " " + target + " :" + message + "\r\n", &client);
	}
	else
	{
		Client *targetClient = findClientByNick(target);
		if (!targetClient)
		{
			if (!notice)
				sendError(client, "401", target + " :No such nick/channel");
			return;
		}
		queueMessage(*targetClient, prefixFor(client) + " " + (notice ? "NOTICE" : "PRIVMSG") + " " + target + " :" + message + "\r\n");
	}
}

void IrcServer::sendChannelTopic(Client &client, Channel &channel)
{
	if (channel.hasTopic)
		sendNumeric(client, "332", channel.name + " :" + channel.topic);
	else
		sendNumeric(client, "331", channel.name + " :No topic is set");
}

void IrcServer::broadcastMembershipNames(Channel &channel, Client &requester)
{
	std::string names;
	for (std::set<Client *>::iterator it = channel.members.begin(); it != channel.members.end(); ++it)
	{
		if (!names.empty())
			names += " ";
		if (channel.operators.find(*it) != channel.operators.end())
			names += "@";
		names += (*it)->nickSet ? (*it)->nick : "*";
	}
	sendNumeric(requester, "353", "= " + channel.name + " :" + names);
	sendNumeric(requester, "366", channel.name + " :End of NAMES list");
}

void IrcServer::cmdTopic(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2)
	{
		sendError(client, "461", "TOPIC :Not enough parameters");
		return;
	}
	Channel *channel = findChannel(args[1]);
	if (!channel)
	{
		sendError(client, "403", args[1] + " :No such channel");
		return;
	}
	if (args.size() == 2)
	{
		sendChannelTopic(client, *channel);
		return;
	}
	if (channel->topicProtected && channel->operators.find(&client) == channel->operators.end())
	{
		sendError(client, "482", channel->name + " :You're not a channel operator");
		return;
	}
	channel->topic = args[2];
	channel->hasTopic = true;
	broadcastToChannel(*channel, prefixFor(client) + " TOPIC " + channel->name + " :" + channel->topic + "\r\n");
}

void IrcServer::applyChannelMode(Channel &channel, Client &actor, char sign, char mode, const std::string &param)
{
	switch (mode)
	{
		case 'i':
			channel.inviteOnly = (sign == '+');
			break;
		case 't':
			channel.topicProtected = (sign == '+');
			break;
		case 'k':
			if (sign == '+')
			{
				channel.hasKey = true;
				channel.key = param;
			}
			else
			{
				channel.hasKey = false;
				channel.key.clear();
			}
			break;
		case 'l':
			if (sign == '+')
			{
				char *end = NULL;
				errno = 0;
				unsigned long value = std::strtoul(param.c_str(), &end, 10);
				if (errno == ERANGE || !end || *end != '\0' || value == 0)
				{
					sendError(actor, "461", "MODE :Invalid limit");
					return;
				}
				channel.limit = static_cast<std::size_t>(value);
				channel.hasLimit = true;
			}
			else
			{
				channel.hasLimit = false;
				channel.limit = 0;
			}
			break;
		case 'o':
		{
			Client *target = findClientByNick(param);
			if (!target || channel.members.find(target) == channel.members.end())
				return;
			if (sign == '+')
				channel.operators.insert(target);
			else
				channel.operators.erase(target);
			break;
		}
		default:
			break;
	}
}

void IrcServer::setChannelMode(Channel &channel, Client &actor, const std::string &modeString, const std::vector<std::string> &params)
{
	if (channel.operators.find(&actor) == channel.operators.end())
	{
		sendError(actor, "482", channel.name + " :You're not a channel operator");
		return;
	}
	char sign = '+';
	std::size_t paramIndex = 0;
	for (std::size_t i = 0; i < modeString.size(); ++i)
	{
		char c = modeString[i];
		if (c == '+' || c == '-')
		{
			sign = c;
			continue;
		}
		std::string param;
		if ((c == 'k' && sign == '+') || (c == 'l' && sign == '+') || c == 'o')
		{
			if (paramIndex >= params.size())
				break;
			param = params[paramIndex++];
		}
		applyChannelMode(channel, actor, sign, c, param);
	}
	broadcastToChannel(channel, prefixFor(actor) + " MODE " + channel.name + " " + modeString + (params.empty() ? "" : " " + joinTokens(params, 0)) + "\r\n");
}

void IrcServer::cmdMode(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 2)
	{
		sendError(client, "461", "MODE :Not enough parameters");
		return;
	}
	Channel *channel = findChannel(args[1]);
	if (!channel)
	{
		sendError(client, "403", args[1] + " :No such channel");
		return;
	}
	if (args.size() == 2)
	{
		std::string modes = "+";
		if (channel->inviteOnly) modes += "i";
		if (channel->topicProtected) modes += "t";
		if (channel->hasKey) modes += "k";
		if (channel->hasLimit) modes += "l";
		sendNumeric(client, "324", channel->name + " " + modes);
		return;
	}
	setChannelMode(*channel, client, args[2], std::vector<std::string>(args.begin() + 3, args.end()));
}

void IrcServer::cmdKick(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 3)
	{
		sendError(client, "461", "KICK :Not enough parameters");
		return;
	}
	Channel *channel = findChannel(args[1]);
	Client *target = findClientByNick(args[2]);
	if (!channel || !target)
	{
		sendError(client, "401", args[2] + " :No such nick/channel");
		return;
	}
	if (channel->operators.find(&client) == channel->operators.end())
	{
		sendError(client, "482", channel->name + " :You're not a channel operator");
		return;
	}
	if (channel->members.find(target) == channel->members.end())
		return;
	std::string msg = args.size() >= 4 ? args[3] : client.nick;
	broadcastToChannel(*channel, prefixFor(client) + " KICK " + channel->name + " " + target->nick + " :" + msg + "\r\n");
	channel->members.erase(target);
	channel->operators.erase(target);
	target->joinedChannels.erase(channel->name);
	destroyChannelIfEmpty(channel->name);
}

void IrcServer::cmdInvite(Client &client, const std::vector<std::string> &args)
{
	if (args.size() < 3)
	{
		sendError(client, "461", "INVITE :Not enough parameters");
		return;
	}
	Channel *channel = findChannel(args[2]);
	Client *target = findClientByNick(args[1]);
	if (!channel || !target)
	{
		sendError(client, "401", args[1] + " :No such nick/channel");
		return;
	}
	if (channel->operators.find(&client) == channel->operators.end())
	{
		sendError(client, "482", channel->name + " :You're not a channel operator");
		return;
	}
	channel->invited.insert(target);
	queueMessage(client, prefixForServer() + " 341 " + client.nick + " " + target->nick + " " + channel->name + "\r\n");
	queueMessage(*target, prefixFor(client) + " INVITE " + target->nick + " :" + channel->name + "\r\n");
}

void IrcServer::broadcastToChannel(Channel &channel, const std::string &message, Client *except)
{
	for (std::set<Client *>::iterator it = channel.members.begin(); it != channel.members.end(); ++it)
	{
		if (*it == except)
			continue;
		queueMessage(**it, message);
	}
}
