# ft_irc

`ft_irc` is a small IRC server written in C++ for the 42 curriculum.  
It accepts TCP clients, parses IRC-like commands, manages channels, and relays messages between connected users.

## Overview

This project reproduces the core behavior of an IRC server:

- multiple simultaneous clients
- nickname and user registration
- private messages and channel messages
- channel creation and membership management
- channel modes, invitations, kicks, and topics
- server replies using IRC numeric responses

The implementation is centered around a non-blocking event loop so the server can handle reads and writes for every connected client in a single process.

## Knowledge needed

To implement this project, you should be comfortable with:

- C++98 and the STL
- TCP/IP socket programming
- non-blocking I/O
- `poll()`-based event loops
- basic IRC command syntax and reply numerics
- state management with maps, sets, and buffers

## Design idea

The server is organized around three main entities:

- **IrcServer**: owns the listening socket, the connected clients, and the active channels
- **Client**: stores registration state, input/output buffers, nickname, username, and joined channels
- **Channel**: stores membership, operators, invitations, topic, and mode flags

The main loop follows this flow:

1. accept new connections
2. read incoming data from clients
3. split input into IRC lines
4. dispatch each command to the proper handler
5. queue replies in the client output buffer
6. flush pending data with `POLLOUT`

This structure keeps the implementation simple while supporting multiple clients at once.

## Implemented features

### Connection and registration

- `PASS`
- `NICK`
- `USER`
- `PING`
- `PONG`
- `QUIT`

Clients must provide the correct password and register with nick + user before using channel commands.

### Channel management

- `JOIN`
- `PART`
- automatic channel creation
- automatic channel cleanup when empty

### Messaging

- `PRIVMSG`
- `NOTICE`
- private user-to-user messages
- channel broadcasts

### Channel control

- `TOPIC`
- `MODE`
- `KICK`
- `INVITE`

Supported channel modes:

- `i` invite-only
- `t` topic protected
- `k` channel key
- `l` user limit
- `o` operator privilege

## Technical notes

- sockets are set to non-blocking mode
- the server uses `poll()` to monitor the listening socket and all clients
- incoming data is buffered until a full `\r\n` line is available
- outgoing data is buffered so partial writes are handled safely
- channel membership, invitations, and operators are tracked with STL containers
- replies follow the IRC numeric format used by common IRC clients

## Usage

Run the server with:

```bash
./ircserv <port> <password>
```

Example:

```bash
./ircserv 6667 secret
```

Then connect with an IRC client using the same port and password.

## Project notes

This repository contains the source code in `src/` and `include/`.  
The current implementation focuses on the core IRC server behavior required for the project and keeps the architecture intentionally small and readable.

