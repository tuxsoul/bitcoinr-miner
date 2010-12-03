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

#define NOMINMAX

#include "remoteminer.h"
#include "base64.h"
#include "../cryptopp/misc.h"

#include <ctime>
#include <cstring>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <netdb.h>
#endif

const int BITCOINMINERREMOTE_THREADINDEX=5;
const int BITCOINMINERREMOTE_HASHESPERMETA=2000000;

#ifdef _WIN32
bool BitcoinMinerRemoteServer::m_wsastartup=false;
#endif

RemoteClientConnection::RemoteClientConnection(const SOCKET sock, sockaddr_storage &addr, const int addrlen):m_socket(sock),m_addr(addr),m_addrlen(addrlen),m_gotclienthello(false),m_connecttime(time(0)),m_lastactive(time(0)),m_lastverifiedmetahash(0)
{
	m_tempbuffer.resize(4096,0);
}

RemoteClientConnection::~RemoteClientConnection()
{
	Disconnect();
	for(std::vector<sentwork>::iterator i=m_sentwork.begin(); i!=m_sentwork.end(); i++)
	{
		if((*i).m_pblock!=0)
		{
			delete (*i).m_pblock;
		}
	}
}

void RemoteClientConnection::ClearOldSentWork(const int sec)
{
	for(std::vector<sentwork>::iterator i=m_sentwork.begin(); i!=m_sentwork.end();)
	{
		if(difftime(time(0),(*i).m_senttime)>=sec)
		{
			if((*i).m_pblock)
			{
				delete (*i).m_pblock;
			}
			i=m_sentwork.erase(i);
		}
		else
		{
			i++;
		}
	}
}

const bool RemoteClientConnection::Disconnect()
{
	m_sendbuffer.clear();
	m_receivebuffer.clear();
	if(IsConnected())
	{
		myclosesocket(m_socket);
	}
	m_socket=INVALID_SOCKET;
	m_gotclienthello=false;
	return true;
}

const std::string RemoteClientConnection::GetAddress(const bool withport) const
{
	std::string address("");
#ifdef _WIN32
	std::vector<TCHAR> buffer(128,0);
	DWORD bufferlen=buffer.size();
	WSAAddressToString((sockaddr *)&m_addr,m_addrlen,0,&buffer[0],&bufferlen);
	if(bufferlen>0)
	{
		address.append(buffer.begin(),buffer.begin()+bufferlen);
	}
#else
	std::vector<char> buffer(128,0);
	int bufferlen=buffer.size();
	struct sockaddr *sa=(sockaddr *)&m_addr;
	switch(sa->sa_family)
	{
	case AF_INET:
			inet_ntop(AF_INET,&(((struct sockaddr_in *)sa)->sin_addr),&buffer[0],bufferlen);
			break;
	case AF_INET6:
			inet_ntop(AF_INET6,&(((struct sockaddr_in6 *)sa)->sin6_addr),&buffer[0],bufferlen);
			break;
	}
	if(bufferlen>0)
	{
		address.append(buffer.begin(),buffer.begin()+bufferlen);
	}
#endif

	if(withport==false)
	{
		std::string::size_type pos=address.find_last_of(':');
		if(pos!=std::string::npos)
		{
			address.erase(pos);
		}
	}

	return address;
}

const int64 RemoteClientConnection::GetCalculatedKHashRateFromBestHash(const int sec, const int minsec) const
{
	// we find how many hashes were necessary to get the best hash, and total this number for all best hashes reported in the time period
	// then we divide by the time period to get khash/s
	int64 hashes=0;
	int64 bits=0;
	time_t now=time(0);
	time_t earliest=now;

	for(std::vector<sentwork>::const_iterator i=m_sentwork.begin(); i!=m_sentwork.end(); i++)
	{
		for(std::vector<metahash>::const_iterator j=(*i).m_metahashes.begin(); j!=(*i).m_metahashes.end(); j++)
		{
			if(difftime(now,(*j).m_senttime)<=sec)
			{
				if(earliest>(*j).m_senttime)
				{
					earliest=(*j).m_senttime;
				}
				// find number of 0 bits on the right
				//bits=0;
				for(int i=7; i>=0; i--)
				{
					unsigned int ch=((unsigned int *)&(*j).m_besthash)[i];
					for(int j=31; j>=0; j--)
					{
						if(((ch >> j) & 0x1)!=0)
						{
							i=0;
							j=0;
							continue;
						}
						else
						{
							bits++;
						}
					}
				}
				//hashes+=static_cast<int64>(static_cast<int64>(2) << bits);
				hashes++;
			}
		}
	}

	long double averagenbits=0;
	if(hashes>0)
	{
		averagenbits=static_cast<long double>(bits)/static_cast<long double>(hashes);
	}
	return static_cast<int64>(::pow(static_cast<long double>(2),averagenbits)/static_cast<long double>(1000));
}

const int64 RemoteClientConnection::GetCalculatedKHashRateFromMetaHash(const int sec, const int minsec) const
{
	int64 hash=0;
	time_t now=time(0);
	time_t earliest=now;
	for(std::vector<sentwork>::const_iterator i=m_sentwork.begin(); i!=m_sentwork.end(); i++)
	{
		for(std::vector<metahash>::const_iterator j=(*i).m_metahashes.begin(); j!=(*i).m_metahashes.end(); j++)
		{
			if(difftime(now,(*j).m_senttime)<=sec)
			{
				if(earliest>(*j).m_senttime)
				{
					earliest=(*j).m_senttime;
				}
				hash+=BITCOINMINERREMOTE_HASHESPERMETA;
			}
		}
	}

	int s=(std::min)(static_cast<int>(now-earliest),sec);
	s=(std::max)(s,minsec);
	int64 denom=(static_cast<int64>(s)*static_cast<int64>(1000));
	if(denom!=0)
	{
		return hash/denom;
	}
	else
	{
		return 0;
	}
}

const bool RemoteClientConnection::GetNewestSentWorkWithMetaHash(sentwork &work) const
{
	bool found=false;
	time_t newest=0;
	for(std::vector<sentwork>::const_iterator i=m_sentwork.begin(); i!=m_sentwork.end(); i++)
	{
		if((*i).m_senttime>=newest && (*i).m_metahashes.size()>0)
		{
			work=(*i);
			newest=(*i).m_senttime;
			found=true;
		}
	}
	return found;
}

const bool RemoteClientConnection::GetSentWorkByBlock(const std::vector<unsigned char> &block, sentwork **work)
{
	for(std::vector<sentwork>::iterator i=m_sentwork.begin(); i!=m_sentwork.end(); i++)
	{
		if((*i).m_block==block)
		{
			*work=&(*i);
			return true;
		}
	}

	return false;
}

const bool RemoteClientConnection::MessageReady() const
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

const bool RemoteClientConnection::ProtocolError() const
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

const bool RemoteClientConnection::ReceiveMessage(RemoteMinerMessage &message)
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

void RemoteClientConnection::SendMessage(const RemoteMinerMessage &message)
{
	message.PushWireData(m_sendbuffer);
}

void RemoteClientConnection::SetWorkVerified(sentwork &work)
{
	for(std::vector<sentwork>::reverse_iterator i=m_sentwork.rbegin(); i!=m_sentwork.rend(); i++)
	{
		if((*i).m_block==work.m_block && (*i).m_metahashes.size()>0)
		{
			(*i).m_metahashes[(*i).m_metahashes.size()-1].m_verified=true;
			return;
		}
	}
}

const bool RemoteClientConnection::SocketReceive()
{
	bool received=false;
	if(IsConnected())
	{
		int rval=::recv(GetSocket(),&m_tempbuffer[0],m_tempbuffer.size(),0);
		if(rval>0)
		{
			m_receivebuffer.insert(m_receivebuffer.end(),m_tempbuffer.begin(),m_tempbuffer.begin()+rval);
			received=true;
			m_lastactive=time(0);
		}
		else
		{
			Disconnect();
		}
	}
	return received;
}

const bool RemoteClientConnection::SocketSend()
{
	bool sent=false;
	if(IsConnected() && m_sendbuffer.size()>0)
	{
		int rval=::send(GetSocket(),&m_sendbuffer[0],m_sendbuffer.size(),0);
		if(rval>0)
		{
			m_sendbuffer.erase(m_sendbuffer.begin(),m_sendbuffer.begin()+rval);
			m_lastactive=time(0);
		}
		else
		{
			Disconnect();
		}
	}
	return sent;
}

BitcoinMinerRemoteServer::BitcoinMinerRemoteServer():m_bnExtraNonce(0)
{
#ifdef _WIN32
	if(m_wsastartup==false)
	{
		WSAData wsadata;
		WSAStartup(MAKEWORD(2,2),&wsadata);
		m_wsastartup=true;
	}
#endif
	ReadBanned("banned.txt");
};

BitcoinMinerRemoteServer::~BitcoinMinerRemoteServer()
{
	printf("BitcoinMinerRemoteServer::~BitcoinMinerRemoteServer()\n");

	// stop listening
	for(std::vector<SOCKET>::iterator i=m_listensockets.begin(); i!=m_listensockets.end(); i++)
	{
		myclosesocket((*i));
	}

	// disconnect all clients
	for(std::vector<RemoteClientConnection *>::iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		(*i)->Disconnect();
		delete (*i);
	}
};

void BitcoinMinerRemoteServer::BlockToJson(const CBlock *block, json_spirit::Object &obj)
{
	obj.push_back(json_spirit::Pair("hash", block->GetHash().ToString().c_str()));
	obj.push_back(json_spirit::Pair("ver", block->nVersion));
	obj.push_back(json_spirit::Pair("prev_block", block->hashPrevBlock.ToString().c_str()));
	obj.push_back(json_spirit::Pair("mrkl_root", block->hashMerkleRoot.ToString().c_str()));
	obj.push_back(json_spirit::Pair("time", (uint64_t)block->nTime));
	obj.push_back(json_spirit::Pair("bits", (uint64_t)block->nBits));
	obj.push_back(json_spirit::Pair("nonce", (uint64_t)block->nNonce));
	obj.push_back(json_spirit::Pair("n_tx", (int)block->vtx.size()));

	json_spirit::Array tx;
	for (int i = 0; i < block->vtx.size(); i++) {
		json_spirit::Object txobj;

	txobj.push_back(json_spirit::Pair("hash", block->vtx[i].GetHash().ToString().c_str()));
	txobj.push_back(json_spirit::Pair("ver", block->vtx[i].nVersion));
	txobj.push_back(json_spirit::Pair("vin_sz", (int)block->vtx[i].vin.size()));
	txobj.push_back(json_spirit::Pair("vout_sz", (int)block->vtx[i].vout.size()));
	txobj.push_back(json_spirit::Pair("lock_time", (uint64_t)block->vtx[i].nLockTime));

	json_spirit::Array tx_vin;
	for (int j = 0; j < block->vtx[i].vin.size(); j++) {
		json_spirit::Object vino;

		json_spirit::Object vino_outpt;

		vino_outpt.push_back(json_spirit::Pair("hash",
    		block->vtx[i].vin[j].prevout.hash.ToString().c_str()));
		vino_outpt.push_back(json_spirit::Pair("n", (uint64_t)block->vtx[i].vin[j].prevout.n));

		vino.push_back(json_spirit::Pair("prev_out", vino_outpt));

		if (block->vtx[i].vin[j].prevout.IsNull())
    		vino.push_back(json_spirit::Pair("coinbase", HexStr(
			block->vtx[i].vin[j].scriptSig.begin(),
			block->vtx[i].vin[j].scriptSig.end(), false).c_str()));
		else
    		vino.push_back(json_spirit::Pair("scriptSig", 
			block->vtx[i].vin[j].scriptSig.ToString().c_str()));
		if (block->vtx[i].vin[j].nSequence != UINT_MAX)
    		vino.push_back(json_spirit::Pair("sequence", (uint64_t)block->vtx[i].vin[j].nSequence));

		tx_vin.push_back(vino);
	}

	json_spirit::Array tx_vout;
	for (int j = 0; j < block->vtx[i].vout.size(); j++) {
		json_spirit::Object vouto;

		vouto.push_back(json_spirit::Pair("value",
    		(double)block->vtx[i].vout[j].nValue / (double)COIN));
		vouto.push_back(json_spirit::Pair("scriptPubKey", 
		block->vtx[i].vout[j].scriptPubKey.ToString().c_str()));

		tx_vout.push_back(vouto);
	}

	txobj.push_back(json_spirit::Pair("in", tx_vin));
	txobj.push_back(json_spirit::Pair("out", tx_vout));

	tx.push_back(txobj);
	}

	obj.push_back(json_spirit::Pair("tx", tx));

	json_spirit::Array mrkl;
	for (int i = 0; i < block->vMerkleTree.size(); i++)
		mrkl.push_back(block->vMerkleTree[i].ToString().c_str());

	obj.push_back(json_spirit::Pair("mrkl_tree", mrkl));
}

const bool BitcoinMinerRemoteServer::DecodeBase64(const std::string &encoded, std::vector<unsigned char> &decoded)
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

const bool BitcoinMinerRemoteServer::EncodeBase64(const std::vector<unsigned char> &data, std::string &encoded)
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

const int64 BitcoinMinerRemoteServer::GetAllClientsCalculatedKHashFromBest() const
{
	int64 rval=0;
	for(std::vector<RemoteClientConnection *>::const_iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		rval+=(*i)->GetCalculatedKHashRateFromBestHash(600);
	}
	return rval;
}

const int64 BitcoinMinerRemoteServer::GetAllClientsCalculatedKHashFromMeta() const
{
	int64 rval=0;
	for(std::vector<RemoteClientConnection *>::const_iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		rval+=(*i)->GetCalculatedKHashRateFromMetaHash();
	}
	return rval;
}

RemoteClientConnection *BitcoinMinerRemoteServer::GetOldestNonVerifiedMetaHashClient()
{
	RemoteClientConnection *client=0;
	time_t oldest=time(0);
	for(std::vector<RemoteClientConnection *>::const_iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		if((*i)->GetLastVerifiedMetaHash()<=oldest)
		{
			client=(*i);
			oldest=(*i)->GetLastVerifiedMetaHash();	
		}
	}
	return client;
}

void BitcoinMinerRemoteServer::ReadBanned(const std::string &filename)
{
	std::vector<char> buff(129,0);
	std::string host("");
	FILE *infile=fopen("banned.txt","r");
	if(infile)
	{
		while(fgets(&buff[0],buff.size()-1,infile))
		{
			host="";
			for(std::vector<char>::iterator i=buff.begin(); i!=buff.end() && (*i)!=0 && (*i)!='\r' && (*i)!='\n'; i++)
			{
				host+=(*i);
			}
			if(host!="")
			{
				m_banned.insert(host);
			}
		}
		fclose(infile);
	}
}

const bool BitcoinMinerRemoteServer::StartListen(const std::string &bindaddr, const std::string &bindport)
{

	SOCKET sock;
	int rval;
	struct addrinfo hint,*result,*current;
	result=current=NULL;
	memset(&hint,0,sizeof(hint));
	hint.ai_socktype=SOCK_STREAM;
	hint.ai_protocol=IPPROTO_TCP;
	hint.ai_flags=AI_PASSIVE;
	
	rval=getaddrinfo(bindaddr.c_str(),bindport.c_str(),&hint,&result);
	if(rval==0)
	{
		for(current=result; current!=NULL; current=current->ai_next)
		{
			sock=socket(current->ai_family,current->ai_socktype,current->ai_protocol);
			if(sock!=INVALID_SOCKET)
			{
				#ifndef _WIN32
				const int optval=1;
				setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof(optval));
				#endif
				if(::bind(sock,current->ai_addr,current->ai_addrlen)==0)
				{
					if(listen(sock,10)==0)
					{
						m_listensockets.push_back(sock);
					}
					else
					{
						myclosesocket(sock);
					}
				}
				else
				{
					myclosesocket(sock);
				}
			}
		}
	}

	if(result)
	{
		freeaddrinfo(result);
	}

	if(m_listensockets.size()==0)
	{
		printf("Remote server couldn't listen on any interface\n");
	}
	else
	{
		printf("Remote server listening on %d interfaces\n",m_listensockets.size());
	}

	return (m_listensockets.size()>0);
};

void BitcoinMinerRemoteServer::SendServerHello(RemoteClientConnection *client, const int metahashrate)
{
	json_spirit::Object obj;
	obj.push_back(json_spirit::Pair("type",static_cast<int>(RemoteMinerMessage::MESSAGE_TYPE_SERVERHELLO)));
	obj.push_back(json_spirit::Pair("serverversion",BITCOINMINERREMOTE_SERVERVERSIONSTR));
	obj.push_back(json_spirit::Pair("metahashrate",static_cast<int>(metahashrate)));
	client->SendMessage(RemoteMinerMessage(obj));
}

void BitcoinMinerRemoteServer::SendServerStatus()
{
	json_spirit::Object obj;
	obj.push_back(json_spirit::Pair("type",static_cast<int>(RemoteMinerMessage::MESSAGE_TYPE_SERVERSTATUS)));
	obj.push_back(json_spirit::Pair("time",static_cast<int64>(time(0))));
	obj.push_back(json_spirit::Pair("clients",static_cast<int64>(m_clients.size())));
	obj.push_back(json_spirit::Pair("khashmeta",static_cast<int64>(GetAllClientsCalculatedKHashFromMeta())));
	obj.push_back(json_spirit::Pair("khashbest",static_cast<int64>(GetAllClientsCalculatedKHashFromBest())));
	for(std::vector<RemoteClientConnection *>::iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		json_spirit::Object messobj(obj);
		messobj.push_back(json_spirit::Pair("yourkhashmeta",(*i)->GetCalculatedKHashRateFromMetaHash()));
		messobj.push_back(json_spirit::Pair("yourkhashbest",(*i)->GetCalculatedKHashRateFromBestHash()));
		(*i)->SendMessage(RemoteMinerMessage(messobj));
	}
}

void BitcoinMinerRemoteServer::SendWork(RemoteClientConnection *client)
{
	const unsigned int SHA256InitState[8] ={0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	// we don't use CReserveKey for now because it removes the reservation when the key goes out of scope
	CKey key;
	key.MakeNewKey();
	CBlockIndex* pindexPrev = pindexBest;
	unsigned int nBits = GetNextWorkRequired(pindexPrev);
	CTransaction txNew;
	txNew.vin.resize(1);
	txNew.vin[0].prevout.SetNull();
	txNew.vin[0].scriptSig << nBits << ++m_bnExtraNonce;
	txNew.vout.resize(1);
	txNew.vout[0].scriptPubKey << key.GetPubKey() << OP_CHECKSIG;

    //
    // Create new block
    //
    CBlock *pblock=new CBlock();
    if(!pblock)
    {
		return;
	}

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

	// Collect memory pool transactions into the block
	int64 nFees = 0;
	CRITICAL_BLOCK(cs_main)
	CRITICAL_BLOCK(cs_mapTransactions)
	{
		CTxDB txdb("r");
		map<uint256, CTxIndex> mapTestPool;
		vector<char> vfAlreadyAdded(mapTransactions.size());
		uint64 nBlockSize = 1000;
		int nBlockSigOps = 100;
		bool fFoundSomething = true;
		while (fFoundSomething)
		{
			fFoundSomething = false;
			unsigned int n = 0;
			for (map<uint256, CTransaction>::iterator mi = mapTransactions.begin(); mi != mapTransactions.end(); ++mi, ++n)
			{
				if (vfAlreadyAdded[n])
					continue;
				CTransaction& tx = (*mi).second;
				if (tx.IsCoinBase() || !tx.IsFinal())
					continue;
				unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK);
				if (nBlockSize + nTxSize >= MAX_BLOCK_SIZE_GEN)
					continue;
				int nTxSigOps = tx.GetSigOpCount();
				if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
					continue;

				// Transaction fee based on block size
				int64 nMinFee = tx.GetMinFee(nBlockSize);

				map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
				if (!tx.ConnectInputs(txdb, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, nFees, false, true, nMinFee))
					continue;
				swap(mapTestPool, mapTestPoolTmp);

				pblock->vtx.push_back(tx);
				nBlockSize += nTxSize;
				nBlockSigOps += nTxSigOps;
				vfAlreadyAdded[n] = true;
				fFoundSomething = true;
			}
		}
	}
	pblock->nBits = nBits;
	pblock->vtx[0].vout[0].nValue = GetBlockValue(pindexPrev->nHeight+1, nFees);

	// add output for each connected client proportional to their khash
	for(std::vector<RemoteClientConnection *>::const_iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		uint160 ch=0;
		if((*i)->GetRequestedRecipientAddress(ch))
		{
			if((*i)->GetCalculatedKHashRateFromMetaHash()>0 && GetAllClientsCalculatedKHashFromMeta()>0)
			{
				double khashfrac=static_cast<double>((*i)->GetCalculatedKHashRateFromMetaHash())/static_cast<double>(GetAllClientsCalculatedKHashFromMeta());
				CTxOut out;
				out.scriptPubKey << OP_DUP << OP_HASH160 << ch << OP_EQUALVERIFY << OP_CHECKSIG;
				out.nValue=GetBlockValue(pindexPrev->nHeight+1, nFees)*khashfrac;
				if(out.nValue>pblock->vtx[0].vout[0].nValue)
				{
					out.nValue=pblock->vtx[0].vout[0].nValue;
				}
				pblock->vtx[0].vout[0].nValue-=out.nValue;
				pblock->vtx[0].vout.push_back(out);
			}
		}
	}

	printf("Sending block to remote client  nBits=%u\n",pblock->nBits);
	pblock->print();

	//
	// Prebuild hash buffer
	//
	struct tmpworkspace
	{
		struct unnamed2
		{
			int nVersion;
			uint256 hashPrevBlock;
			uint256 hashMerkleRoot;
			unsigned int nTime;
			unsigned int nBits;
			unsigned int nNonce;
		}
		block;
		unsigned char pchPadding0[64];
		uint256 hash1;
		unsigned char pchPadding1[64];
	};
	char tmpbuf[sizeof(tmpworkspace)+16];
	tmpworkspace& tmp = *(tmpworkspace*)alignup<16>(tmpbuf);

	tmp.block.nVersion       = pblock->nVersion;
	tmp.block.hashPrevBlock  = pblock->hashPrevBlock  = (pindexPrev ? pindexPrev->GetBlockHash() : 0);
	tmp.block.hashMerkleRoot = pblock->hashMerkleRoot = pblock->BuildMerkleTree();
	tmp.block.nTime          = pblock->nTime          = max((pindexPrev ? pindexPrev->GetMedianTimePast()+1 : 0), GetAdjustedTime());
	tmp.block.nBits          = pblock->nBits          = nBits;
	tmp.block.nNonce         = pblock->nNonce         = 0;

	unsigned int nBlocks0 = FormatHashBlocks(&tmp.block, sizeof(tmp.block));
	unsigned int nBlocks1 = FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

	// Byte swap all the input buffer
	for (int i = 0; i < sizeof(tmp)/4; i++)
		((unsigned int*)&tmp)[i] = CryptoPP::ByteReverse(((unsigned int*)&tmp)[i]);

	// Precalc the first half of the first hash, which stays constant
	uint256 midstatebuf[2];
	uint256& midstate = *alignup<16>(midstatebuf);
	SHA256Transform(&midstate, &tmp.block, SHA256InitState);

	uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

	// create and send the message to the client
	std::string blockstr("");
	std::string midstatestr("");
	std::string targetstr(hashTarget.GetHex());

	std::vector<unsigned char> blockbuff(64,0);
	::memcpy(&blockbuff[0],((char *)&tmp.block)+64,64);
	std::vector<unsigned char> midbuff(32,0);
	::memcpy(&midbuff[0],(char *)&midstate,32);

	EncodeBase64(blockbuff,blockstr);
	EncodeBase64(midbuff,midstatestr);

	json_spirit::Object obj;
	obj.push_back(json_spirit::Pair("type",RemoteMinerMessage::MESSAGE_TYPE_SERVERSENDWORK));
	obj.push_back(json_spirit::Pair("block",blockstr));
	obj.push_back(json_spirit::Pair("midstate",midstatestr));
	obj.push_back(json_spirit::Pair("target",targetstr));

	// send complete block with transactions so client can verify
	json_spirit::Object fullblock;
	BlockToJson(pblock,fullblock);
	obj.push_back(json_spirit::Pair("fullblock",fullblock));

	client->SendMessage(RemoteMinerMessage(obj));

	// save this block with the client connection so we can verify the metahashes generated by the client
	RemoteClientConnection::sentwork sw;
	sw.m_key=key;
	sw.m_block=blockbuff;
	sw.m_midstate=midbuff;
	sw.m_target=hashTarget;
	sw.m_senttime=time(0);
	sw.m_pblock=pblock;
	sw.m_indexprev=pindexPrev;
	client->GetSentWork().push_back(sw);

	// clear out old work sent to client (sent 15 minutes or older)
	client->ClearOldSentWork(900);
}

void BitcoinMinerRemoteServer::SendWorkToAllClients()
{
	for(std::vector<RemoteClientConnection *>::iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		SendWork((*i));
	}
}

const bool BitcoinMinerRemoteServer::Step()
{
	int rval;
	fd_set readfs;
	fd_set writefs;
	struct timeval tv;
	std::vector<SOCKET>::iterator listeni;
	SOCKET highsocket;

	// reset values
	highsocket=0;
	tv.tv_sec=0;
	tv.tv_usec=100;

	// clear fd set
	FD_ZERO(&readfs);

	// put all listen sockets on the fd set
	for(listeni=m_listensockets.begin(); listeni!=m_listensockets.end(); listeni++)
	{
		FD_SET((*listeni),&readfs);
		if((*listeni)>highsocket)
		{
			highsocket=(*listeni);
		}
	}

	// see if any connections are waiting
	rval=select(highsocket+1,&readfs,0,0,&tv);

	// check for new connections
	if(rval>0)
	{
		for(listeni=m_listensockets.begin(); listeni!=m_listensockets.end(); listeni++)
		{
			if(FD_ISSET((*listeni),&readfs))
			{
				SOCKET newsock;
				struct sockaddr_storage addr;
				socklen_t addrlen=sizeof(addr);
				newsock=accept((*listeni),(struct sockaddr *)&addr,&addrlen);
				if(newsock!=INVALID_SOCKET)
				{
					RemoteClientConnection *newclient=new RemoteClientConnection(newsock,addr,addrlen);
					if(m_banned.find(newclient->GetAddress(false))!=m_banned.end())
					{
						printf("Banned client %s connected.  Disconnecting.\n",newclient->GetAddress().c_str());
						newclient->Disconnect();
						delete newclient;
					}
					else
					{
						m_clients.push_back(newclient);
						printf("Remote client %s connected\n",newclient->GetAddress().c_str());
					}
				}
			}
		}
	}

	// send and receive on existing connections
	highsocket=0;
	FD_ZERO(&readfs);
	FD_ZERO(&writefs);
	for(std::vector<RemoteClientConnection *>::iterator i=m_clients.begin(); i!=m_clients.end(); i++)
	{
		if((*i)->IsConnected())
		{
			FD_SET((*i)->GetSocket(),&readfs);
			if((*i)->GetSocket()>highsocket)
			{
				highsocket=(*i)->GetSocket();
			}

			if((*i)->SendBufferSize()>0)
			{
				FD_SET((*i)->GetSocket(),&writefs);
			}
		}
	}

	rval=select(highsocket+1,&readfs,&writefs,0,&tv);

	if(rval>0)
	{
		for(std::vector<RemoteClientConnection *>::iterator i=m_clients.begin(); i!=m_clients.end(); i++)
		{
			if((*i)->IsConnected() && FD_ISSET((*i)->GetSocket(),&readfs))
			{
				(*i)->SocketReceive();
			}
			if((*i)->IsConnected() && FD_ISSET((*i)->GetSocket(),&writefs))
			{
				(*i)->SocketSend();
			}
		}
	}

	// remove any disconnected clients, or clients with too much data in the receive buffer
	for(std::vector<RemoteClientConnection *>::iterator i=m_clients.begin(); i!=m_clients.end(); )
	{
		if((*i)->IsConnected()==false || (*i)->ReceiveBufferSize()>(1024*1024))
		{
			printf("Remote client %s disconnected\n",(*i)->GetAddress().c_str());
			delete (*i);
			i=m_clients.erase(i);
		}
		else
		{
			i++;
		}
	}

	return true;

}

const bool VerifyBestHash(const RemoteClientConnection::sentwork &work, const uint256 &besthash, const unsigned int besthashnonce)
{
	const unsigned int SHA256InitState[8] ={0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	
	uint256 tempbuff[4];
	uint256 &temphash=*alignup<16>(tempbuff);
	uint256 hashbuff[4];
	uint256 &hash=*alignup<16>(hashbuff);
	unsigned char midbuff[256]={0};
	unsigned char blockbuff[256]={0};
	unsigned char *midbuffptr=alignup<16>(midbuff);
	unsigned char *blockbuffptr=alignup<16>(blockbuff);
	unsigned int *nonce=(unsigned int *)(blockbuffptr+12);

	for(int i=0; i<4; i++)
	{
		tempbuff[i]=0;
		hashbuff[i]=0;
	}
	temphash=0;
	hash=0;

	FormatHashBlocks(&temphash,sizeof(temphash));
	for(int i=0; i<64/4; i++)
	{
		((unsigned int*)&temphash)[i] = CryptoPP::ByteReverse(((unsigned int*)&temphash)[i]);
	}
	
	::memcpy(blockbuffptr,&(work.m_block[0]),work.m_block.size());
	::memcpy(midbuffptr,&(work.m_midstate[0]),work.m_midstate.size());

	(*nonce)=besthashnonce;

	SHA256Transform(&temphash,blockbuffptr,midbuffptr);
	SHA256Transform(&hash,&temphash,SHA256InitState);

	for (int i = 0; i < sizeof(hash)/4; i++)
	{
		((unsigned int*)&hash)[i] = CryptoPP::ByteReverse(((unsigned int*)&hash)[i]);
	}

	return (hash==besthash);
}

const bool VerifyMetaHash(const RemoteClientConnection::sentwork &work)
{
	const unsigned int SHA256InitState[8] ={0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	
	uint256 tempbuff[4];
	uint256 &temphash=*alignup<16>(tempbuff);
	uint256 hashbuff[4];
	uint256 &hash=*alignup<16>(hashbuff);
	unsigned char midbuff[256]={0};
	unsigned char blockbuff[256]={0};
	unsigned char *midbuffptr=alignup<16>(midbuff);
	unsigned char *blockbuffptr=alignup<16>(blockbuff);
	unsigned int *nonce=(unsigned int *)(blockbuffptr+12);
	std::vector<unsigned char> metahash(BITCOINMINERREMOTE_HASHESPERMETA,0);
	std::vector<unsigned char>::size_type metahashpos=0;

	for(int i=0; i<4; i++)
	{
		tempbuff[i]=0;
		hashbuff[i]=0;
	}
	temphash=0;
	hash=0;
	
	std::vector<RemoteClientConnection::metahash>::size_type mhpos=work.m_metahashes.size()-1;

	FormatHashBlocks(&temphash,sizeof(temphash));
	for(int i=0; i<64/4; i++)
	{
		((unsigned int*)&temphash)[i] = CryptoPP::ByteReverse(((unsigned int*)&temphash)[i]);
	}
	
	::memcpy(blockbuffptr,&(work.m_block[0]),work.m_block.size());
	::memcpy(midbuffptr,&(work.m_midstate[0]),work.m_midstate.size());
	
	for((*nonce)=work.m_metahashes[mhpos].m_startnonce; (*nonce)<work.m_metahashes[mhpos].m_startnonce+BITCOINMINERREMOTE_HASHESPERMETA; (*nonce)++,metahashpos++)
	{
		SHA256Transform(&temphash,blockbuffptr,midbuffptr);
		SHA256Transform(&hash,&temphash,SHA256InitState);
		
		metahash[metahashpos]=((unsigned char *)&hash)[0];
	}
	
	std::vector<unsigned char> digest(SHA256_DIGEST_LENGTH,0);
	SHA256(&metahash[0],metahash.size(),&digest[0]);
	
	return (digest==work.m_metahashes[mhpos].m_metahash);

}

const bool VerifyFoundHash(RemoteClientConnection *client, const std::vector<unsigned char> &block, const unsigned int foundnonce)
{
	const unsigned int SHA256InitState[8] ={0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	
	uint256 tempbuff[4];
	uint256 &temphash=*alignup<16>(tempbuff);
	uint256 hashbuff[4];
	uint256 &hash=*alignup<16>(hashbuff);
	unsigned char midbuff[256]={0};
	unsigned char blockbuff[256]={0};
	unsigned char *midbuffptr=alignup<16>(midbuff);
	unsigned char *blockbuffptr=alignup<16>(blockbuff);
	unsigned int *nonce=(unsigned int *)(blockbuffptr+12);

	for(int i=0; i<4; i++)
	{
		tempbuff[i]=0;
		hashbuff[i]=0;
	}
	temphash=0;
	hash=0;

	FormatHashBlocks(&temphash,sizeof(temphash));
	for(int i=0; i<64/4; i++)
	{
		((unsigned int*)&temphash)[i] = CryptoPP::ByteReverse(((unsigned int*)&temphash)[i]);
	}
	
	RemoteClientConnection::sentwork *work;
	if(client->GetSentWorkByBlock(block,&work))
	{
		
		if(work->m_pblock)
		{
			::memset(blockbuffptr,0,64);
			::memset(midbuffptr,0,32);
			::memcpy(blockbuffptr,&work->m_block[0],work->m_block.size());
			::memcpy(midbuffptr,&work->m_midstate[0],work->m_midstate.size());

			(*nonce)=foundnonce;
			
			SHA256Transform(&temphash,blockbuffptr,midbuffptr);
			SHA256Transform(&hash,&temphash,SHA256InitState);

			for (int i = 0; i < sizeof(hash)/4; i++)
			{
				((unsigned int*)&hash)[i] = CryptoPP::ByteReverse(((unsigned int*)&hash)[i]);
			}

			work->m_pblock->nNonce=CryptoPP::ByteReverse(foundnonce);

			if(hash==work->m_pblock->GetHash() && hash<=work->m_target)
			{
                CRITICAL_BLOCK(cs_main)
                {
                    if (work->m_indexprev == pindexBest)
                    {
						// save the key
						AddKey(work->m_key);

                        // Track how many getdata requests this block gets
                        CRITICAL_BLOCK(cs_mapRequestCount)
                            mapRequestCount[work->m_pblock->GetHash()] = 0;

                        // Process this block the same as if we had received it from another node
                        if (!ProcessBlock(NULL, work->m_pblock))
                            printf("ERROR in VerifyFoundHash, ProcessBlock, block not accepted\n");
                    	
						// pblock is deleted by ProcessBlock, so we need to zero the pointer in work so it won't be deleted twice
						work->m_pblock=0;

                    }
                }
				
				return true;
			}
			else
			{
				printf("VerifyFoundHash, client %s sent data that doesn't hash as expected\n",client->GetAddress().c_str());
			}

		}
	}
	return false;
}

void ThreadBitcoinMinerRemote(void* parg)
{
    try
    {
        vnThreadsRunning[BITCOINMINERREMOTE_THREADINDEX]++;
        BitcoinMinerRemote();
        vnThreadsRunning[BITCOINMINERREMOTE_THREADINDEX]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[BITCOINMINERREMOTE_THREADINDEX]--;
        PrintException(&e, "ThreadBitcoinMinerRemote()");
    } catch (...) {
        vnThreadsRunning[BITCOINMINERREMOTE_THREADINDEX]--;
        PrintException(NULL, "ThreadBitcoinMinerRemote()");
    }
    UIThreadCall(bind(CalledSetStatusBar, "", 0));
    nHPSTimerStart = 0;
    if (vnThreadsRunning[BITCOINMINERREMOTE_THREADINDEX] == 0)
        dHashesPerSec = 0;
    printf("ThreadBitcoinMinerRemote exiting, %d threads remaining\n", vnThreadsRunning[BITCOINMINERREMOTE_THREADINDEX]);
}

void BitcoinMinerRemote()
{
	std::string bindaddr("127.0.0.1");
	std::string bindport("8335");
	std::string remotepassword("");
	BitcoinMinerRemoteServer serv;
	time_t laststatusbarupdate=time(0);
	time_t lastverified=time(0);
	time_t lastserverstatus=time(0);

	if(mapArgs.count("-remotebindaddr"))
	{
		bindaddr=mapArgs["-remotebindaddr"];
	}
	if(mapArgs.count("-remotebindport"))
	{
		bindport=mapArgs["-remotebindport"];
	}
	if(mapArgs.count("-remotepassword"))
	{
		remotepassword=mapArgs["-remotepassword"];
	}

	serv.StartListen(bindaddr,bindport);

	SetThreadPriority(THREAD_PRIORITY_LOWEST);

	while(fGenerateBitcoins)
	{
		serv.Step();

		// handle messages

		for(std::vector<RemoteClientConnection *>::iterator i=serv.Clients().begin(); i!=serv.Clients().end(); i++)
		{
			while((*i)->MessageReady() && !(*i)->ProtocolError())
			{
				RemoteMinerMessage message;
				int type=RemoteMinerMessage::MESSAGE_TYPE_NONE;
				if((*i)->ReceiveMessage(message) && message.GetValue().type()==json_spirit::obj_type)
				{
					json_spirit::Value val=json_spirit::find_value(message.GetValue().get_obj(),"type");
					if(val.type()==json_spirit::int_type)
					{
						type=val.get_int();
						if((*i)->GotClientHello()==false && type!=RemoteMinerMessage::MESSAGE_TYPE_CLIENTHELLO)
						{
							printf("Client sent first message other than clienthello\n");
							(*i)->Disconnect();
						}
						else if((*i)->GotClientHello()==false && type==RemoteMinerMessage::MESSAGE_TYPE_CLIENTHELLO)
						{

							json_spirit::Value pval=json_spirit::find_value(message.GetValue().get_obj(),"address");
							if(pval.type()==json_spirit::str_type)
							{
								uint160 address;
								address.SetHex(pval.get_str());
								(*i)->SetRequestedRecipientAddress(address);
							}

							pval=json_spirit::find_value(message.GetValue().get_obj(),"password");
							if(pval.type()==json_spirit::str_type && pval.get_str()==remotepassword)
							{
								printf("Got clienthello from client %s\n",(*i)->GetAddress().c_str());
								(*i)->SetGotClientHello(true);
								serv.SendServerHello((*i),BITCOINMINERREMOTE_HASHESPERMETA);
								// also send work right away
								serv.SendWork((*i));
							}
							else
							{
								printf("Client %s didn't send correct password.  Disconnecting.\n",(*i)->GetAddress().c_str());
								(*i)->Disconnect();
							}

						}
						else if(type==RemoteMinerMessage::MESSAGE_TYPE_CLIENTGETWORK)
						{
							// only send new work if it has been at least 30 seconds since the last work
							if((*i)->GetSentWork().size()==0 || difftime(time(0),(*i)->GetSentWork()[(*i)->GetSentWork().size()-1].m_senttime)>=30)
							{
								serv.SendWork((*i));
							}
						}
						else if(type==RemoteMinerMessage::MESSAGE_TYPE_CLIENTMETAHASH)
						{
							std::vector<unsigned char> block;
							std::vector<unsigned char> digest;
							unsigned int nonce=0;
							uint256 besthash=~0;
							unsigned int besthashnonce=0;

							json_spirit::Value val=json_spirit::find_value(message.GetValue().get_obj(),"block");
							if(val.type()==json_spirit::str_type)
							{
								BitcoinMinerRemoteServer::DecodeBase64(val.get_str(),block);
							}
							val=json_spirit::find_value(message.GetValue().get_obj(),"digest");
							if(val.type()==json_spirit::str_type)
							{
								BitcoinMinerRemoteServer::DecodeBase64(val.get_str(),digest);
							}
							val=json_spirit::find_value(message.GetValue().get_obj(),"nonce");
							if(val.type()==json_spirit::int_type)
							{
								nonce=val.get_int64();
							}
							val=json_spirit::find_value(message.GetValue().get_obj(),"besthash");
							if(val.type()==json_spirit::str_type)
							{
								besthash.SetHex(val.get_str());
							}
							val=json_spirit::find_value(message.GetValue().get_obj(),"besthashnonce");
							if(val.type()==json_spirit::int_type)
							{
								besthashnonce=val.get_int64();
							}
							RemoteClientConnection::sentwork *work;
							if((*i)->GetSentWorkByBlock(block,&work))
							{
								if(work->CheckNonceOverlap(nonce,BITCOINMINERREMOTE_HASHESPERMETA)==false && besthashnonce>=nonce && besthashnonce<nonce+BITCOINMINERREMOTE_HASHESPERMETA)
								{
									if(VerifyBestHash(*work,besthash,besthashnonce)==true)
									{
										RemoteClientConnection::metahash mh;
										mh.m_metahash=digest;
										mh.m_senttime=time(0);
										mh.m_startnonce=nonce;
										mh.m_verified=false;
										mh.m_besthash=besthash;
										mh.m_besthashnonce=besthashnonce;
										work->m_metahashes.push_back(mh);
									}
									else
									{
										printf("Couldn't verify best hash from client %s\n",(*i)->GetAddress().c_str());
									}
								}
								else
								{
									printf("Detected nonce overlap from client %s\n",(*i)->GetAddress().c_str());
								}
							}
							else
							{
								printf("Client %s sent metahash for block we don't know about!\n",(*i)->GetAddress().c_str());
							}
						}
						else if(type==RemoteMinerMessage::MESSAGE_TYPE_CLIENTFOUNDHASH)
						{
							SetThreadPriority(THREAD_PRIORITY_NORMAL);

							std::vector<unsigned char> block;
							int64 nonce=0;
							
							json_spirit::Value val=json_spirit::find_value(message.GetValue().get_obj(),"block");
							if(val.type()==json_spirit::str_type)
							{
								BitcoinMinerRemoteServer::DecodeBase64(val.get_str(),block);
							}
							val=json_spirit::find_value(message.GetValue().get_obj(),"nonce");
							if(val.type()==json_spirit::int_type)
							{
								nonce=val.get_int();
							}
							
							if(VerifyFoundHash((*i),block,nonce)==true)
							{
								// send ALL clients new block to work on
								serv.SendWorkToAllClients();
							}
							SetThreadPriority(THREAD_PRIORITY_LOWEST);
						}
						else
						{
							printf("Unhandled message type (%d) from client %s\n",type,(*i)->GetAddress().c_str());
						}
					}
					else
					{
						printf("Unexpected json type detected when finding message type.  Disconnecting client %s.\n",(*i)->GetAddress().c_str());
						(*i)->Disconnect();
					}
				}
				else
				{
					printf("There was an error receiving a message from a client.  Disconnecting the client %s.\n",(*i)->GetAddress().c_str());
					(*i)->Disconnect();
				}

			}

			if((*i)->ProtocolError())
			{
				printf("There was protcol error from client %s.  Disconnecting.\n",(*i)->GetAddress().c_str());
				(*i)->Disconnect();
			}

			// send new block every 2 minutes
			if((*i)->GetSentWork().size()>0)
			{
				if(difftime(time(0),(*i)->GetSentWork()[(*i)->GetSentWork().size()-1].m_senttime)>=120)
				{
					serv.SendWork((*i));
				}
			}
		}

		if(serv.Clients().size()==0)
		{
			Sleep(100);
		}

		if(difftime(time(0),laststatusbarupdate)>=10)
		{
			int64 clients=serv.Clients().size();
			int64 khashmeta=serv.GetAllClientsCalculatedKHashFromMeta();
			int64 khashbest=serv.GetAllClientsCalculatedKHashFromBest();
			//std::string strStatus = strprintf(" %"PRI64d" clients    %"PRI64d" khash/s meta     %"PRI64d" khash/s best",clients,khashmeta,khashbest);
			std::string strStatus = strprintf(" %"PRI64d" clients    %"PRI64d" khash/s meta",clients,khashmeta);
			UIThreadCall(bind(CalledSetStatusBar, strStatus, 0));
			laststatusbarupdate=time(0);
		}

		// check metahash of a client every 10 seconds
		if(difftime(time(0),lastverified)>=10)
		{
			RemoteClientConnection *client=serv.GetOldestNonVerifiedMetaHashClient();
			if(client)
			{
				RemoteClientConnection::sentwork work;
				if(client->GetNewestSentWorkWithMetaHash(work))
				{
					if(VerifyMetaHash(work)==false)
					{
						printf("Client %s failed metahash verification!\n",client->GetAddress().c_str());
					}
					client->SetWorkVerified(work);
				}
				client->SetLastVerifiedMetaHash(lastverified);
			}
			lastverified=time(0);
		}
		
		// send server status to all connected clients every minute
		if(difftime(time(0),lastserverstatus)>=60)
		{
			serv.SendServerStatus();
			lastserverstatus=time(0);
		}

	}	// while fGenerateBitcoins

}