/**
    Copyright (C) 2010  puddinpop

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
**/

#include "remoteminerclient.h"
#include "remoteminermessage.h"
#include "base64.h"

#include <iostream>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <netdb.h>
#endif

#ifdef _WIN32
bool RemoteMinerClient::m_wsastartup=false;
#endif

RemoteMinerClient::RemoteMinerClient():m_socket(INVALID_SOCKET),m_tempbuffer(4096,0),m_gotserverhello(false)
{
#ifdef _WIN32
	if(m_wsastartup==false)
	{
		WSAData wsadata;
		WSAStartup(MAKEWORD(2,2),&wsadata);
		m_wsastartup=true;
	}
#endif
	m_midbuffptr=alignup<16>(m_currentmidbuff);
	m_blockbuffptr=alignup<16>(m_currentblockbuff);
	m_nonce=(unsigned int *)(m_blockbuffptr+12);
}

RemoteMinerClient::~RemoteMinerClient()
{
	Disconnect();
}

const bool RemoteMinerClient::Connect(const std::string &server, const std::string &port)
{
	m_sendbuffer.clear();
	m_receivebuffer.clear();

	if(IsConnected()==true)
	{
		Disconnect();
	}

	int rval=-1;
	std::ostringstream portstring;
	addrinfo hint,*result,*current;

	result=current=0;
	std::memset(&hint,0,sizeof(hint));
	hint.ai_socktype=SOCK_STREAM;

	rval=getaddrinfo(server.c_str(),port.c_str(),&hint,&result);

	if(result)
	{
		for(current=result; current!=0 && m_socket==INVALID_SOCKET; current=current->ai_next)
		{

			m_socket=socket(current->ai_family,current->ai_socktype,current->ai_protocol);

			if(m_socket!=INVALID_SOCKET)
			{
				rval=connect(m_socket,current->ai_addr,current->ai_addrlen);
				if(rval==-1)
				{
					Disconnect();
				}
			}

		}

		freeaddrinfo(result);
	}

	if(rval==0)
	{
		return true;
	}
	else
	{
		return false;
	}
}

const bool RemoteMinerClient::DecodeBase64(const std::string &encoded, std::vector<unsigned char> &decoded) const
{
	if(encoded.size()>0)
	{
		int dlen=((encoded.size()*3)/4)+4;
		decoded.resize(dlen,0);
		std::vector<unsigned char> src(encoded.begin(),encoded.end());
		if(base64_decode(&decoded[0],&dlen,&src[0],src.size())==0)
		{
			decoded.resize(dlen);
			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		decoded.resize(0);
		return true;
	}
}

const bool RemoteMinerClient::Disconnect()
{
	m_sendbuffer.clear();
	m_receivebuffer.clear();
	if(IsConnected())
	{
		myclosesocket(m_socket);
		m_socket=INVALID_SOCKET;
	}
	return true;
}

const bool RemoteMinerClient::EncodeBase64(const std::vector<unsigned char> &data, std::string &encoded) const
{
	if(data.size()>0)
	{
		int dstlen=((data.size()*4)/3)+4;
		std::vector<unsigned char> dst(dstlen,0);
		if(base64_encode(&dst[0],&dstlen,&data[0],data.size())==0)
		{
			dst.resize(dstlen);
			encoded.assign(dst.begin(),dst.end());
			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		encoded=std::string("");
		return true;
	}
}

void RemoteMinerClient::HandleMessage(const RemoteMinerMessage &message)
{
	json_spirit::Value tval=json_spirit::find_value(message.GetValue().get_obj(),"type");
	if(tval.type()==json_spirit::int_type)
	{
		std::cout << "Got message " << tval.get_int() << " from server." << std::endl;
		if(tval.get_int()==RemoteMinerMessage::MESSAGE_TYPE_SERVERHELLO)
		{
			m_gotserverhello=true;
			tval=json_spirit::find_value(message.GetValue().get_obj(),"metahashrate");
			if(tval.type()==json_spirit::int_type)
			{
				m_metahashstartblock.clear();
				m_metahashstartblock.insert(m_metahashstartblock.end(),128,0);
				m_metahash.clear();
				m_metahash.resize(tval.get_int());
				m_metahashpos=0;
			}
			tval=json_spirit::find_value(message.GetValue().get_obj(),"serverversion");
			if(tval.type()==json_spirit::str_type)
			{
				std::cout << "Server version " << tval.get_str() << std::endl;
			}
		}
		else if(tval.get_int()==RemoteMinerMessage::MESSAGE_TYPE_SERVERSENDWORK)
		{
			tval=json_spirit::find_value(message.GetValue().get_obj(),"block");
			if(tval.type()==json_spirit::str_type)
			{
				DecodeBase64(tval.get_str(),m_nextblock);
			}
			tval=json_spirit::find_value(message.GetValue().get_obj(),"target");
			if(tval.type()==json_spirit::str_type)
			{
				m_nexttarget.SetHex(tval.get_str());
			}
			tval=json_spirit::find_value(message.GetValue().get_obj(),"midstate");
			if(tval.type()==json_spirit::str_type)
			{
				DecodeBase64(tval.get_str(),m_nextmidstate);
			}
			if(m_havework==false)
			{
				m_currenttarget=m_nexttarget;
				::memcpy(m_midbuffptr,&m_nextmidstate[0],32);
				::memcpy(m_blockbuffptr,&m_nextblock[0],64);
				m_metahashstartblock=m_nextblock;
				m_metahashpos=0;
				m_metahashstartnonce=0;
				(*m_nonce)=0;
			}
			m_havework=true;
		}
		else if(tval.get_int()==RemoteMinerMessage::MESSAGE_TYPE_SERVERSTATUS)
		{
			int64 clients=0;
			int64 khashmeta=0;
			int64 khashbest=0;
			int64 clientkhashmeta=0;
			tval=json_spirit::find_value(message.GetValue().get_obj(),"clients");
			if(tval.type()==json_spirit::int_type)
			{
				clients=tval.get_int();
			}
			tval=json_spirit::find_value(message.GetValue().get_obj(),"khashmeta");
			if(tval.type()==json_spirit::int_type)
			{
				khashmeta=tval.get_int();
			}
			tval=json_spirit::find_value(message.GetValue().get_obj(),"khashbest");
			if(tval.type()==json_spirit::int_type)
			{
				khashbest=tval.get_int();
			}
			tval=json_spirit::find_value(message.GetValue().get_obj(),"yourkhashmeta");
			if(tval.type()==json_spirit::int_type)
			{
				clientkhashmeta=tval.get_int();
			}
			
			//std::cout << "Server Status : " << clients << " clients, " << khashmeta << " khash/s m " << khashbest << " khash/s b  " << std::endl;
			std::cout << "Server Status : " << clients << " clients, " << khashmeta << " khash/s" << std::endl;
			std::cout << "Server reports my khash/s as " << clientkhashmeta << std::endl;
		}
	}
	else
	{
		std::cout << "Server sent invalid message.  Disconnecting." << std::endl;
	}
}

const bool RemoteMinerClient::MessageReady() const
{
	if(m_receivebuffer.size()>3 && m_receivebuffer[0]==REMOTEMINER_PROTOCOL_VERSION)
	{
		unsigned short messagesize=(m_receivebuffer[1] << 8) & 0xff00;
		messagesize|=(m_receivebuffer[2]) & 0xff;

		if(m_receivebuffer.size()>=3+messagesize)
		{
			return true;
		}
	}

	return false;
}

const bool RemoteMinerClient::ProtocolError() const
{
	if(m_receivebuffer.size()>0 && m_receivebuffer[0]!=REMOTEMINER_PROTOCOL_VERSION)
	{
		return true;
	}
	else
	{
		return false;
	}
}

const bool RemoteMinerClient::ReceiveMessage(RemoteMinerMessage &message)
{
	if(MessageReady())
	{
		unsigned short messagesize=(m_receivebuffer[1] << 8) & 0xff00;
		messagesize|=(m_receivebuffer[2]) & 0xff;

		std::string objstr(m_receivebuffer.begin()+3,m_receivebuffer.begin()+3+messagesize);
		json_spirit::Value value;
		bool jsonread=json_spirit::read(objstr,value);
		if(jsonread)
		{
			message=RemoteMinerMessage(value);
		}
		m_receivebuffer.erase(m_receivebuffer.begin(),m_receivebuffer.begin()+3+messagesize);
		return jsonread;
	}
	return false;
}

void RemoteMinerClient::Run(const std::string &server, const std::string &port, const std::string &password, const std::string &address)
{
	static const unsigned int SHA256InitState[8] ={0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint256 tempbuff[4];
	uint256 &temphash=*alignup<16>(tempbuff);
	uint256 hashbuff[4];
	uint256 &hash=*alignup<16>(hashbuff);
	uint256 lasttarget=0;

	uint256 besthash=~(uint256(0));
	unsigned int besthashnonce=0;

	SetThreadPriority(THREAD_PRIORITY_LOWEST);

	FormatHashBlocks(&temphash,sizeof(temphash));
	for(int i=0; i<64/4; i++)
	{
		((unsigned int*)&temphash)[i] = CryptoPP::ByteReverse(((unsigned int*)&temphash)[i]);
	}

	while(true)
	{
		if(IsConnected()==false)
		{
			std::cout << "Attempting to connect to " << server << ":" << port << std::endl;
			if(Connect(server,port))
			{
				m_gotserverhello=false;
				m_havework=false;
				m_metahashpos=0;
				lasttarget=0;
				(*m_nonce)=0;
				m_metahashstartnonce=0;
				besthash=~(uint256(0));
				besthashnonce=0;
				std::cout << "Connected to " << server << ":" << port << std::endl;
				SendClientHello(password,address);
			}
			else
			{
				Sleep(1000);
			}
		}
		else
		{
			if(m_gotserverhello==false || m_metahashpos%10000==0)
			{
				Update();
				while(MessageReady() && !ProtocolError())
				{
					RemoteMinerMessage message;
					if(ReceiveMessage(message))
					{
						if(message.GetValue().type()==json_spirit::obj_type)
						{
							HandleMessage(message);
						}
						else
						{
							std::cout << "Unexpected json type sent by server.  Disconnecting." << std::endl;
							Disconnect();
						}
					}
				}
				if(ProtocolError())
				{
					std::cout << "Protocol error.  Disconnecting." << std::endl;
					Disconnect();
				}
			}

			if(m_havework)
			{
				// do 1000 hashes at a time
				for(unsigned int i=0; i<1000 && m_metahashpos<m_metahash.size(); i++)
				{
					SHA256Transform(&temphash,m_blockbuffptr,m_midbuffptr);
					SHA256Transform(&hash,&temphash,SHA256InitState);

					m_metahash[m_metahashpos++]=((unsigned char *)&hash)[0];

					if((((unsigned short*)&hash)[14]==0) && (((unsigned short*)&hash)[15]==0))
					{
						for (int i = 0; i < sizeof(hash)/4; i++)
						{
							((unsigned int*)&hash)[i] = CryptoPP::ByteReverse(((unsigned int*)&hash)[i]);
						}
						
						if(hash<=m_currenttarget)
						{
							std::cout << "Found block!" << std::endl;

							// send hash found to server
							SendFoundHash(m_metahashstartblock,(*m_nonce));
						}

						if(hash<besthash)
						{
							besthash=hash;
							besthashnonce=(*m_nonce);
						}
					}
					// hash isn't bytereversed yet, but besthash already is
					else if(CryptoPP::ByteReverse(((unsigned int*)&hash)[7])<=((unsigned int *)&besthash)[7])
					{
						for (int i = 0; i < sizeof(hash)/4; i++)
						{
							((unsigned int*)&hash)[i] = CryptoPP::ByteReverse(((unsigned int*)&hash)[i]);
						}
						if(hash<besthash)
						{
							besthash=hash;
							besthashnonce=(*m_nonce);
						}
					}

					(*m_nonce)++;
				}

				if(m_metahashpos>=m_metahash.size())
				{

					std::vector<unsigned char> digest(SHA256_DIGEST_LENGTH,0);
					SHA256(&m_metahash[0],m_metahash.size(),&digest[0]);

					// send the metahash to the server and request a new block if the nonce is approaching the limit
					SendMetaHash(m_metahashstartblock,m_metahashstartnonce,digest,besthash,besthashnonce);
					if((*m_nonce)>4000000000)
					{
						std::cout << "Requesting a new block" << std::endl;
						SendWorkRequest();
					}

					// reset the nonce to 0 if this is a new block
					if(m_nextblock!=m_metahashstartblock)
					{
						(*m_nonce)=0;
					}
					// set meta nonce first because memcpy will overwrite it
					m_metahashstartnonce=(*m_nonce);
					m_currenttarget=m_nexttarget;
					::memcpy(m_midbuffptr,&m_nextmidstate[0],32);
					::memcpy(m_blockbuffptr,&m_nextblock[0],64);
					m_metahashstartblock=m_nextblock;
					m_metahashpos=0;
					// set nonce again because it was overwritten by memcpy
					(*m_nonce)=m_metahashstartnonce;

					if(lasttarget!=m_currenttarget)
					{
						std::cout << "Target Hash : " << m_currenttarget.ToString() << std::endl;
						lasttarget=m_currenttarget;
					}

					std::cout << "Best : " << besthash.ToString() << std::endl;
					besthash=~(uint256(0));
					besthashnonce=0;
				}

			}

		}
	}

}

void RemoteMinerClient::SendClientHello(const std::string &password, const std::string &address)
{
	json_spirit::Object obj;
	
	obj.push_back(json_spirit::Pair("type",RemoteMinerMessage::MESSAGE_TYPE_CLIENTHELLO));
	obj.push_back(json_spirit::Pair("password",password));
	if(address!="")
	{
		uint160 h160;
		if(AddressToHash160(address.c_str(),h160))
		{
			obj.push_back(json_spirit::Pair("address",h160.GetHex()));
		}
	}
	
	SendMessage(RemoteMinerMessage(obj));
}

void RemoteMinerClient::SendFoundHash(const std::vector<unsigned char> &block, const unsigned int nonce)
{
	json_spirit::Object obj;
	std::string blockstr("");
	
	EncodeBase64(block,blockstr);
	
	obj.push_back(json_spirit::Pair("type",static_cast<int>(RemoteMinerMessage::MESSAGE_TYPE_CLIENTFOUNDHASH)));
	obj.push_back(json_spirit::Pair("block",blockstr));
	obj.push_back(json_spirit::Pair("nonce",static_cast<int64>(nonce)));
	
	SendMessage(RemoteMinerMessage(obj));
}

void RemoteMinerClient::SendMessage(const RemoteMinerMessage &message)
{
	message.PushWireData(m_sendbuffer);
}

void RemoteMinerClient::SendMetaHash(const std::vector<unsigned char> &block, const unsigned int startnonce, const std::vector<unsigned char> &digest, const uint256 &besthash, const unsigned int besthashnonce)
{
	std::string blockstr("");
	std::string digeststr("");

	EncodeBase64(block,blockstr);
	EncodeBase64(digest,digeststr);
	json_spirit::Object obj;
	obj.push_back(json_spirit::Pair("type",static_cast<int>(RemoteMinerMessage::MESSAGE_TYPE_CLIENTMETAHASH)));
	obj.push_back(json_spirit::Pair("block",blockstr));
	obj.push_back(json_spirit::Pair("nonce",static_cast<int64>(startnonce)));
	obj.push_back(json_spirit::Pair("digest",digeststr));
	obj.push_back(json_spirit::Pair("besthash",besthash.ToString()));
	obj.push_back(json_spirit::Pair("besthashnonce",static_cast<int64>(besthashnonce)));

	SendMessage(RemoteMinerMessage(obj));
}

void RemoteMinerClient::SendWorkRequest()
{
	json_spirit::Object obj;
	obj.push_back(json_spirit::Pair("type",RemoteMinerMessage::MESSAGE_TYPE_CLIENTGETWORK));

	SendMessage(RemoteMinerMessage(obj));
}

void RemoteMinerClient::SocketReceive()
{
	if(IsConnected())
	{
		int len=::recv(m_socket,&m_tempbuffer[0],m_tempbuffer.size(),0);
		if(len>0)
		{
			m_receivebuffer.insert(m_receivebuffer.end(),m_tempbuffer.begin(),m_tempbuffer.begin()+len);
		}
		else
		{
			Disconnect();
		}
	}
}

void RemoteMinerClient::SocketSend()
{
	if(IsConnected() && m_sendbuffer.size()>0)
	{
		int len=::send(m_socket,&m_sendbuffer[0],m_sendbuffer.size(),0);
		if(len>0)
		{
			m_sendbuffer.erase(m_sendbuffer.begin(),m_sendbuffer.begin()+len);
		}
		else
		{
			Disconnect();
		}
	}
}

const bool RemoteMinerClient::Update()
{
	if(IsConnected())
	{
		m_timeval.tv_sec=0;
		m_timeval.tv_usec=0;

		FD_ZERO(&m_readfs);
		FD_ZERO(&m_writefs);

		FD_SET(m_socket,&m_readfs);

		if(m_sendbuffer.size()>0)
		{
			FD_SET(m_socket,&m_writefs);
		}

		select(m_socket+1,&m_readfs,&m_writefs,0,&m_timeval);

		if(IsConnected() && FD_ISSET(m_socket,&m_readfs))
		{
			SocketReceive();
		}
		if(IsConnected() && FD_ISSET(m_socket,&m_writefs))
		{
			SocketSend();
		}

	}

	if(IsConnected())
	{
		return true;
	}
	else
	{
		return false;
	}

}