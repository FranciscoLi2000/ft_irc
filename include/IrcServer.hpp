#ifndef IRCSERVER_HPP
#define IRCSERVER_HPP

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

class Client;
class Channel;

class Client {
public:
	Client();
	~Client();

	int fd;
	std::string nick;
	std::string user;
	std::string realname;
	bool passOk;
	bool nickSet;
	bool userSet;
	bool registered;
	bool pendingQuit;
	std::string quitMessage;
	std::string inBuffer;
	std::string outBuffer;
	std::set<std::string> joinedChannels;
};

class Channel {
public:
	explicit Channel(const std::string &channelName);
	~Channel();

	std::string name;
	std::string topic;
	bool hasTopic;
	bool inviteOnly;
	bool topicProtected;
	bool hasKey;
	bool hasLimit;
	std::string key;
	std::size_t limit;
	std::set<Client *> members;
	std::set<Client *> operators;
	std::set<Client *> invited;
};

class IrcServer {
public:
	IrcServer(int port, const std::string &password);
	~IrcServer();

	void run();
	static bool isNumber(const std::string &value);

private:
	IrcServer();
	IrcServer(const IrcServer &);
	IrcServer &operator=(const IrcServer &);

	int _port;
	std::string _password;
	int _listenFd;
	bool _running;
	std::map<int, Client *> _clients;
	std::map<std::string, Channel *> _channels;

	void setupSocket();
	void acceptClient();
	void handleClientRead(int fd);
	void handleClientWrite(int fd);
	void disconnectClient(int fd, const std::string &reason, bool notify = true);
	void destroyChannelIfEmpty(std::string name);

	void processLine(Client &client, const std::string &line);
	void handleCommand(Client &client, const std::vector<std::string> &args);

	std::vector<std::string> tokenize(const std::string &line) const;
	static std::string trimCRLF(const std::string &line);
	static std::string toUpper(std::string value);
	static bool isChannelName(const std::string &target);
	static std::string prefixFor(const Client &client);
	static std::string prefixForServer();
	static std::string intToString(int value);
	void queueMessage(Client &client, const std::string &message);
	bool flushClient(Client &client);
	void sendNumeric(Client &client, const std::string &code, const std::string &message);
	void sendError(Client &client, const std::string &code, const std::string &message);
	void sendWelcome(Client &client);

	Client *findClientByNick(const std::string &nick) const;
	Channel *findChannel(const std::string &name) const;
	Channel &getOrCreateChannel(const std::string &name);
	void removeClientFromAllChannels(Client &client, const std::string &quitMsg);

	void cmdPass(Client &client, const std::vector<std::string> &args);
	void cmdNick(Client &client, const std::vector<std::string> &args);
	void cmdUser(Client &client, const std::vector<std::string> &args);
	void cmdPing(Client &client, const std::vector<std::string> &args);
	void cmdPong(Client &client, const std::vector<std::string> &args);
	void cmdQuit(Client &client, const std::vector<std::string> &args);
	void cmdJoin(Client &client, const std::vector<std::string> &args);
	void cmdPart(Client &client, const std::vector<std::string> &args);
	void cmdPrivmsg(Client &client, const std::vector<std::string> &args, bool notice);
	void cmdTopic(Client &client, const std::vector<std::string> &args);
	void cmdMode(Client &client, const std::vector<std::string> &args);
	void cmdKick(Client &client, const std::vector<std::string> &args);
	void cmdInvite(Client &client, const std::vector<std::string> &args);

	void joinChannel(Client &client, Channel &channel, const std::string &key);
	void partChannel(Client &client, Channel &channel, const std::string &message);
	void broadcastToChannel(Channel &channel, const std::string &message, Client *except = NULL);
	void broadcastMembershipNames(Channel &channel, Client &requester);
	void sendChannelTopic(Client &client, Channel &channel);
	void setChannelMode(Channel &channel, Client &actor, const std::string &modeString, const std::vector<std::string> &params);
	void applyChannelMode(Channel &channel, Client &actor, char sign, char mode, const std::string &param);
};

#endif
