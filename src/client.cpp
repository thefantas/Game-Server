#include "client.h"
#include "asyncwork.h"
#include "database.h"

#include <threadpool.h>
#include <Game/packet.h>
#include <threadpool11/threadpool11.hpp>

#include <boost/lockfree/queue.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

using namespace Net;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::open_document;
using bsoncxx::builder::stream::close_document;
using bsoncxx::builder::stream::open_array;
using bsoncxx::builder::stream::close_array;
using bsoncxx::builder::stream::finalize;


extern boost::lockfree::queue<AbstractWork*> asyncWork;

struct PacketHandler
{
	std::string opcode;
	CurrentPhase type;
	WorkType worker;
};

PacketHandler packetHandler[] = {
	{ "Char_NEW",	CurrentPhase::SELECTION_SCREEN, MAKE_WORK(&Client::createCharacter) },
	{ "Char_DEL",	CurrentPhase::SELECTION_SCREEN, MAKE_WORK(&Client::deleteCharacter) },
	{ "select",		CurrentPhase::SELECTION_SCREEN, MAKE_WORK(&Client::gameStartInitialize) },
	{ "game_start", CurrentPhase::SELECTION_SCREEN, MAKE_WORK(&Client::gameStartConfirmation) },
	{ "lbs",		CurrentPhase::INGAME,			MAKE_WORK(&Client::receivedLBS) },
	{ "npinfo",		CurrentPhase::INGAME,			MAKE_WORK(&Client::receivedNPINFO) },

	{ "", CurrentPhase::NONE, nullptr }
};

Client::Client() :
	_currentWork(MAKE_WORK(&Client::handleConnect)),
	_phase(CurrentPhase::SELECTION_SCREEN),
	_currentCharacter(nullptr)
{}

bool Client::workRouter(AbstractWork* work)
{
	// FIXME: Implement other packets!
	if (_currentWork != nullptr)
	{
		return (this->*_currentWork)(work);
	}

	PacketHandler* currentHandler = &packetHandler[0];
	while (currentHandler->worker != nullptr)
	{
		if (currentHandler->opcode == ((ClientWork*)work)->packet().tokens()[1])
		{
			if (currentHandler->type == _phase)
			{
				return (this->*currentHandler->worker)(work);
			}
		}

		++currentHandler;
	}

	return true;
}

bool Client::handleConnect(ClientWork* work)
{
	if (work->packet().tokens().length() == 2)
	{
		_session.setAlive(work->packet().tokens().from_int<uint32_t>(0) + 1);
		_session.setID(work->packet().tokens().from_int<uint32_t>(1));

		_currentWork = MAKE_WORK(&Client::handleUserCredentials);

		return true;
	}

	return false;
}

bool Client::handleUserCredentials(ClientWork* work)
{
	if (work->packet().tokens().length() == 2)
	{
		_username = work->packet().tokens().str(1);
		_currentWork = MAKE_WORK(&Client::handlePasswordCredentials);
		return true;
	}

	return false;
}

bool Client::handlePasswordCredentials(ClientWork* work)
{
	if (work->packet().tokens().length() == 2)
	{
		std::string password = work->packet().tokens().str(1);

		std::cout << "User: " << _username << " PASS: " << password << std::endl;

		auto future = gDB("login")->query<bool>([this, password](mongocxx::database db) {
			bsoncxx::builder::stream::document filter_builder;
			filter_builder << "_id" << _username << "password" << password << "session_id" << (uint16_t)_session.id();

			return db["users"].count(filter_builder.view()) == 1;
		});

		asyncWork.push(new FutureWork<bool>(this, MAKE_WORK(&Client::sendConnectionResult), std::move(future)));

		return true;
	}

	return false;
}

bool Client::sendConnectionResult(FutureWork<bool>* work)
{
	if (work->get())
	{
		auto future = gDB("characters")->query<bool>([this](mongocxx::database db) {
			bsoncxx::builder::stream::document filter_builder;
			filter_builder << "user" << _username;

			_characters.clear();

			auto cursor = db["characters"].find(filter_builder.view());
			for (auto&& doc : cursor)
			{
				_characters.emplace_back(new Character{
					doc["slot"].get_int32(),
					doc["title"].get_utf8().value.to_string(),
					doc["_id"].get_utf8().value.to_string(),
					(Sex)(int)doc["sex"].get_int32(),
					doc["hair"].get_int32(),
					doc["color"].get_int32(),
					doc["level"].get_int32(),
					doc["hp"].get_int32(),
					doc["mp"].get_int32(),
					doc["exp"].get_int32(),
					{ // Profession
						doc["profession"]["level"].get_int32(),
						doc["profession"]["exp"].get_int32()
					}
				});

				std::cout << "New char: " << std::endl <<
					"\t" << _characters.back()->name << std::endl <<
					"\t" << _characters.back()->color << std::endl;
			}

			return true;
		});

		asyncWork.push(new FutureWork<bool>(this, MAKE_WORK(&Client::sendCharactersList), std::move(future)));
		_currentWork = nullptr;
		return true;
	}
	
	return false;
}

bool Client::sendCharactersList(FutureWork<bool>* work)
{
	if (work->get())
	{
		/*<< clist 0 Blipi 0 0 1 4 0 0 2 -1.12.1.8.-1.-1.-1.-1 1  1 1 -1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1 0 0*/
		Packet* clist_start = gFactory->make(PacketType::SERVER_GAME, &_session, NString("clist_start 0"));
		clist_start->send(this);

		// "clist 0 Blipi 0 0 1 4 0 0 2 -1.12.1.8.-1.10.-1.-1 1  1 1 -1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1 0 0"

		Packet* clist_all = gFactory->make(PacketType::SERVER_GAME, &_session, NString());

		for (auto&& pj : _characters)
		{
			Packet* clist = gFactory->make(PacketType::SERVER_GAME, &_session, NString());
			*clist << "clist " << (int)pj->slot << ' ';
			*clist << pj->name << " 0 ";
			*clist << (int)pj->sex << ' ';
			*clist << (int)pj->hair << ' ';
			*clist << (int)pj->color << ' ';
			*clist << "0 0 " << (int)pj->level << ' ';

			if (pj->slot == 0)
			{
				*clist << "-1.12.1.8.-1.-1.-1.-1 1 1  1 -1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1.-1 0 0";
			}
			else
			{
				*clist << "-1.-1.-1.-1.-1.-1.-1.-1 1 1 1 -1 0";
			}

			std::cout << "Sending player " << pj->name << std::endl;
			std::cout << clist->data().get() << std::endl;

			clist_all = *clist_all + *clist;
		}

		Packet* clist_end = gFactory->make(PacketType::SERVER_GAME, &_session, NString("clist_end"));
		clist_all->send(this);
		clist_end->send(this);
		
		return true;
	}
	else
	{
		sendError("User not found");
		return false;
	}
}

bool Client::createCharacter(ClientWork* work)
{
	if (work->packet().tokens().length() != 7)
	{
		return false;
	}

	auto name = work->packet().tokens().str(2);
	auto slot = work->packet().tokens().from_int<uint8_t>(3);
	auto sex = work->packet().tokens().from_int<uint8_t>(4);
	auto hair = work->packet().tokens().from_int<uint8_t>(5);
	auto color = work->packet().tokens().from_int<uint8_t>(6);

	if (name.length() <= 4 || slot > 2 || sex > 1 || hair > 1 || color > 9)
	{
		return false;
	}

	auto future = gDB("characters")->query<bool>([this, name, slot, sex, hair, color](mongocxx::database db) {
		auto character = document{} <<
			"_id" << name <<
			"slot" << slot <<
			"title" << "" <<
			"user" << _username <<
			"sex" << sex <<
			"hair" << hair <<
			"color" << color <<
			"level" << (int)1 <<
			finalize;

		try
		{
			db["characters"].insert_one(character.view());
		}
		catch (std::exception e)
		{
			return false;
		}

		return true;
	});

	asyncWork.push(new FutureWork<bool>(this, MAKE_WORK(&Client::sendConnectionResult), std::move(future)));

	return true;
}

bool Client::deleteCharacter(ClientWork* work)
{
	if (work->packet().tokens().length() == 4)
	{
		int slot = work->packet().tokens().from_int<int>(2);
		std::string password = work->packet().tokens().str(3);

		auto future = gDB("login")->query<int>([this, password, slot](mongocxx::database db) {
			bsoncxx::builder::stream::document filter_builder;
			filter_builder << "_id" << _username << "password" << password;

			if (db["users"].count(filter_builder.view()) == 1)
			{
				return slot;
			}

			return -1;
		});

		asyncWork.push(new FutureWork<int>(this, MAKE_WORK(&Client::confirmDeleteCharacter), std::move(future)));
		return true;
	}

	return false;
}

bool Client::confirmDeleteCharacter(FutureWork<int>* work)
{
	int slot = work->get();

	if (slot >= 0)
	{
		auto future = gDB("characters")->query<bool>([this, slot](mongocxx::database db) {
			bsoncxx::builder::stream::document filter_builder;
			filter_builder << "user" << _username << "slot" << slot;

			db["characters"].delete_one(filter_builder.view());
			return true;
		});

		asyncWork.push(new FutureWork<bool>(this, MAKE_WORK(&Client::sendConnectionResult), std::move(future)));
		return true;
	}
	
	// FIXME: Password was not correct, do not disconnect but send an error
	return false;
}

bool Client::gameStartInitialize(ClientWork* work)
{
	if (work->packet().tokens().length() == 3)
	{
		int slot = work->packet().tokens().from_int<int>(1);
		for (auto&& pj : _characters)
		{
			if (pj->slot == slot)
			{
				_currentCharacter = pj;
				break;
			}
		}
	}

	if (_currentCharacter != nullptr)
	{
		Packet* packet = gFactory->make(PacketType::SERVER_GAME, &_session, NString("OK"));
		packet->send(this);

		return true;
	}

	return false;
}

bool Client::gameStartConfirmation(ClientWork* work)
{
	if (_currentCharacter != nullptr)
	{
		_phase = CurrentPhase::INGAME;
		return true;
	}
	return false;
}

bool Client::receivedLBS(ClientWork* work)
{
	Packet* packet = gFactory->make(PacketType::SERVER_GAME, &_session, NString("tit ") << _currentCharacter->title << " " << _currentCharacter->name);
	packet->send(this);

	packet = gFactory->make(PacketType::SERVER_GAME, &_session, NString("fd 0 1 0 1"));
	packet->send(this);

	_ingameID = 123;

	//c_info[Nombre] - -1 -1 - [INGAME_ID][SLOT][SEXO][PELO][COLOR] 0[LVL] 0 0 0 0 0 0
	Packet* cinfo = gFactory->make(PacketType::SERVER_GAME, &_session, NString("c_info "));
	*cinfo << _currentCharacter->name << ' ';
	*cinfo << "- -1 -1 - ";
	*cinfo << _ingameID << ' ';
	*cinfo << (int)_currentCharacter->slot << ' ';
	*cinfo << (int)_currentCharacter->sex << ' ';
	*cinfo << (int)_currentCharacter->hair << ' ';
	*cinfo << (int)_currentCharacter->color << ' ';
	*cinfo << "0 " << (int)_currentCharacter->level << " 0 0 0 0 0 0";
	cinfo->send(this);

	// lev[LVL][XP][LVL_PROF][XP_PROF][XP_NEXT_LEVEL][XP_NEXT_PROF] 0[NEXT_LVL]
	Packet* lev = gFactory->make(PacketType::SERVER_GAME, &_session, NString("lev "));
	*lev << (int)_currentCharacter->level << ' ' << _currentCharacter->experience << ' ';
	*lev << (int)_currentCharacter->profession.level << ' ' << _currentCharacter->profession.experience << ' ';
	*lev << 300 << ' ' << 500 << ' ';
	*lev << 0 << ' ' << (int)(_currentCharacter->level + 1);
	lev->send(this);

	Packet* stat = gFactory->make(PacketType::SERVER_GAME, &_session, NString("stat "));
	*stat << _currentCharacter->maxHP << ' ' << _currentCharacter->maxHP << ' ';
	*stat << _currentCharacter->maxMP << ' ' << _currentCharacter->maxMP << ' ';
	*stat << 0 << ' ' << 1024;
	stat->send(this);

	Packet* ski = gFactory->make(PacketType::SERVER_GAME, &_session, NString("ski 200 201 200 201 209"));
	ski->send(this);

	Packet* at = gFactory->make(PacketType::SERVER_GAME, &_session, NString("at ") << _ingameID << " 145 25 1 2 0 6 1");
	at->send(this);

	Packet* cmap = gFactory->make(PacketType::SERVER_GAME, &_session, NString("cmap ") << _ingameID << " 0 145 1");
	cmap->send(this);

	Packet* sc = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sc 0 0 30 38 30 4 70 1 0 32 34 41 2 70 0 17 34 19 34 17 0 0 0 0"));
	sc->send(this);

	Packet* cond = gFactory->make(PacketType::SERVER_GAME, &_session, NString("cond 1 ") << _ingameID << " 0 0 11");
	cond->send(this);

	Packet* pairy = gFactory->make(PacketType::SERVER_GAME, &_session, NString("pairy 1 ") << _ingameID << " 0 0 0 0");
	pairy->send(this);

	Packet* rsfi = gFactory->make(PacketType::SERVER_GAME, &_session, NString("rsfi 1 1 0 9 0 9"));
	rsfi->send(this);

	Packet* rank_cool = gFactory->make(PacketType::SERVER_GAME, &_session, NString("rank_cool 0 0 18000"));
	rank_cool->send(this);

	Packet* src = gFactory->make(PacketType::SERVER_GAME, &_session, NString("src 0 0 0 0 0 0 0"));
	src->send(this);

	Packet* exts = gFactory->make(PacketType::SERVER_GAME, &_session, NString("exts 0 48 48 48"));
	exts->send(this);

	Packet* gidx = gFactory->make(PacketType::SERVER_GAME, &_session, NString("gidx 1 ") << _ingameID << " -1 - 0");
	gidx->send(this);

	Packet* mlinfo = gFactory->make(PacketType::SERVER_GAME, &_session, NString("mlinfo 3800 2000 100 0 0 10 0 Abcdefghijk"));
	mlinfo->send(this);

	Packet* p_clear = gFactory->make(PacketType::SERVER_GAME, &_session, NString("p_clear"));
	p_clear->send(this);

	Packet* sc_p_stc = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sc_p_stc 0"));
	sc_p_stc->send(this);

	Packet* p_init = gFactory->make(PacketType::SERVER_GAME, &_session, NString("p_init 0"));
	p_init->send(this);

	Packet* zzim = gFactory->make(PacketType::SERVER_GAME, &_session, NString("zzim"));
	zzim->send(this);

	// 1 = Slot + 1?
	Packet* twk = gFactory->make(PacketType::SERVER_GAME, &_session, NString("twk 1 ") << _ingameID << ' ' << _username << ' ' << _currentCharacter->name << " shtmxpdlfeoqkr");
	twk->send(this);
	
	return true;
}

bool Client::receivedNPINFO(ClientWork* work)
{
	Packet* script = gFactory->make(PacketType::SERVER_GAME, &_session, NString("script 1 27"));
	script->send(this);

	Packet* qstlist = gFactory->make(PacketType::SERVER_GAME, &_session, NString("qstlist 5.1997.1997.19.0.0.0.0.0.0.0.0.0.0.0.0"));
	qstlist->send(this);

	Packet* target = gFactory->make(PacketType::SERVER_GAME, &_session, NString("target 57 149 1 1997"));
	target->send(this);

	Packet* sqst0 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  0 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst0->send(this);
		
	Packet* sqst1 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  1 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst1->send(this);

	Packet* sqst2 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  2 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst2->send(this);

	Packet* sqst3 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  3 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst3->send(this);

	Packet* sqst4 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  4 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst4->send(this);

	Packet* sqst5 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  5 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst5->send(this);

	Packet* sqst6 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("sqst  6 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
	sqst6->send(this);

	Packet* fs = gFactory->make(PacketType::SERVER_GAME, &_session, NString("fs 0"));
	fs->send(this);

	Packet* p_clear = gFactory->make(PacketType::SERVER_GAME, &_session, NString("p_clear"));
	p_clear->send(this);

	Packet* inv0 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("inv 0"));
	inv0->send(this);

	Packet* inv1 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("inv 1"));
	inv1->send(this);

	Packet* inv2 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("inv 2 0.2024.10 1.2081.1"));
	inv2->send(this);

	Packet* inv3 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("inv 3"));
	inv3->send(this);

	Packet* mlobjlst = gFactory->make(PacketType::SERVER_GAME, &_session, NString("mlobjlst"));
	mlobjlst->send(this);

	Packet* inv6 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("inv 6"));
	inv6->send(this);

	Packet* inv7 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("inv 7"));
	inv7->send(this);

	Packet* gold = gFactory->make(PacketType::SERVER_GAME, &_session, NString("gold 0 0"));
	gold->send(this);

	Packet* qslot0 = gFactory->make(PacketType::SERVER_GAME, &_session, NString("qslot 0 1.1.1 0.2.0 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 1.1.16 1.3.1"));
	qslot0->send(this);

	for (int i = 1; i <= 9; ++i)
	{
		Packet* qslot = gFactory->make(PacketType::SERVER_GAME, &_session, NString("qslot ") << i << " 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1 7.7.-1");
		qslot->send(this);
	}
		

	return true;
}

void Client::sendError(std::string&& error)
{
	Packet* errorPacket = gFactory->make(PacketType::SERVER_GAME, &_session, NString("fail ") << error);
	errorPacket->send(this);
}


void Client::onRead(NString packet)
{
	Packet* loginPacket = gFactory->make(PacketType::SERVER_GAME, &_session, packet);
	auto packets = loginPacket->decrypt();
	for (auto data : packets)
	{
		std::cout << ">> " << data.get() << std::endl;
		asyncWork.push(new ClientWork(this, MAKE_WORK(&Client::workRouter), data));
	}
}
