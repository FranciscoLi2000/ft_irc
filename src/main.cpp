#include "IrcServer.hpp"

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		std::cerr << "Usage: " << argv[0] << " <port> <password>" << std::endl;
		return 1;
	}
	if (!IrcServer::isNumber(argv[1]))
	{
		std::cerr << "Error: invalid port" << std::endl;
		return 1;
	}
	const int port = std::atoi(argv[1]);
	if (port <= 0 || port > 65535)
	{
		std::cerr << "Error: invalid port" << std::endl;
		return 1;
	}
	try
	{
		IrcServer server(port, argv[2]);
		server.run();
	}
	catch (const std::exception &e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
