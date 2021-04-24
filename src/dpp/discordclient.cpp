/************************************************************************************
 *
 * D++, A Lightweight C++ library for Discord
 *
 * Copyright 2021 Craig Edwards and D++ contributors 
 * (https://github.com/brainboxdotcc/DPP/graphs/contributors)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ************************************************************************************/
#include <string>
#include <iostream>
#include <fstream>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <dpp/discordclient.h>
#include <dpp/cache.h>
#include <dpp/cluster.h>
#include <thread>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <zlib.h>

#define DEFAULT_GATEWAY		"gateway.discord.gg"
#define PATH_UNCOMPRESSED	"/?v=8&encoding=json"
#define PATH_COMPRESSED		"/?v=8&encoding=json&compress=zlib-stream"
#define DECOMP_BUFFER_SIZE	512 * 1024

namespace dpp {

DiscordClient::DiscordClient(dpp::cluster* _cluster, uint32_t _shard_id, uint32_t _max_shards, const std::string &_token, uint32_t _intents, bool comp)
       : WSClient(DEFAULT_GATEWAY, "443", comp ? PATH_COMPRESSED : PATH_UNCOMPRESSED),
	creator(_cluster),
	shard_id(_shard_id),
	max_shards(_max_shards),
	token(_token),
	last_heartbeat(time(NULL)),
	heartbeat_interval(0),
	reconnects(0),
	resumes(0),
	last_seq(0),
	sessionid(""),
	intents(_intents),
	runner(nullptr),
	compressed(comp),
	decompressed_total(0),
	decomp_buffer(nullptr),
	ready(false)
{
	Connect();
}

DiscordClient::~DiscordClient()
{
	if (runner) {
		runner->join();
		delete runner;
	}
}

uint64_t DiscordClient::GetDeompressedBytesIn()
{
	return decompressed_total;
}

void DiscordClient::SetupZLib()
{
	if (compressed) {
		d_stream.zalloc = (alloc_func)0;
		d_stream.zfree = (free_func)0;
		d_stream.opaque = (voidpf)0;
		if (inflateInit(&d_stream) != Z_OK) {
			throw std::runtime_error("Can't initialise stream compression!");
		}
		this->decomp_buffer = new unsigned char[DECOMP_BUFFER_SIZE];
	}

}

void DiscordClient::EndZLib()
{
	if (compressed) {
		inflateEnd(&d_stream);
		if (this->decomp_buffer) {
			delete[] this->decomp_buffer;
			this->decomp_buffer = nullptr;
		}
	}
}

void DiscordClient::ThreadRun()
{
	SetupZLib();
	do {
		SSLClient::ReadLoop();
		SSLClient::close();
		ready = false;
		message_queue.clear();
		EndZLib();
		SetupZLib();
		SSLClient::Connect();
		WSClient::Connect();
	} while(true);
}

void DiscordClient::Run()
{
	this->runner = new std::thread(&DiscordClient::ThreadRun, this);
	this->thread_id = runner->native_handle();
}

bool DiscordClient::HandleFrame(const std::string &buffer)
{
	std::string& data = (std::string&)buffer;

	/* gzip compression is a special case */
	if (compressed) {
		/* Check that we have a complete compressed frame */
		if ((uint8_t)buffer[buffer.size() - 4] == 0x00 && (uint8_t)buffer[buffer.size() - 3] == 0x00 && (uint8_t)buffer[buffer.size() - 2] == 0xFF
		&& (uint8_t)buffer[buffer.size() - 1] == 0xFF) {
			/* Decompress buffer */
			decompressed.clear();
			d_stream.next_in = (Bytef *)buffer.c_str();
			d_stream.avail_in = buffer.size();
			do {
				int have = 0;
				d_stream.next_out = (Bytef*)decomp_buffer;
				d_stream.avail_out = DECOMP_BUFFER_SIZE;
				int ret = inflate(&d_stream, Z_NO_FLUSH);
				have = DECOMP_BUFFER_SIZE - d_stream.avail_out;
				switch (ret)
				{
					case Z_NEED_DICT:
					case Z_STREAM_ERROR:
						this->Error(6000);
						this->close();
						return true;
					break;
					case Z_DATA_ERROR:
						this->Error(6001);
						this->close();
						return true;
					break;
					case Z_MEM_ERROR:
						this->Error(6002);
						this->close();
						return true;
					break;
					case Z_OK:
						this->decompressed.append((const char*)decomp_buffer, have);
						this->decompressed_total += have;
					break;
					default:
						/* Stub */
					break;
				}
			} while (d_stream.avail_out == 0);
			data = decompressed;
		} else {
			/* No complete compressed frame yet */
			return false;
		}
	}


	log(dpp::ll_trace, fmt::format("R: {}", data));
	json j;
	
	try {
		j = json::parse(data);
	}
	catch (const std::exception &e) {
		log(dpp::ll_error, fmt::format("DiscordClient::HandleFrame {} [{}]", e.what(), data));
		return true;
	}

	if (j.find("s") != j.end() && !j["s"].is_null()) {
		last_seq = j["s"].get<uint64_t>();
	}

	if (j.find("op") != j.end()) {
		uint32_t op = j["op"];

		switch (op) {
			case 9:
				/* Reset session state and fall through to 10 */
				op = 10;
				log(dpp::ll_debug, fmt::format("Failed to resume session {}, will reidentify", sessionid));
				this->sessionid = "";
				this->last_seq = 0;
				/* No break here, falls through to state 10 to cause a reidentify */
			case 10:
				/* Need to check carefully for the existence of this before we try to access it! */
				if (j.find("d") != j.end() && j["d"].find("heartbeat_interval") != j["d"].end() && !j["d"]["heartbeat_interval"].is_null()) {
					this->heartbeat_interval = j["d"]["heartbeat_interval"].get<uint32_t>();
				}

				if (last_seq && !sessionid.empty()) {
					/* Resume */
					log(dpp::ll_debug, fmt::format("Resuming session {} with seq={}", sessionid, last_seq));
					json obj = {
						{ "op", 6 },
						{ "d", {
								{"token", this->token },
								{"session_id", this->sessionid },
								{"seq", this->last_seq }
							}
						}
					};
					this->write(obj.dump());
					resumes++;
				} else {
					/* Full connect */
					while (time(NULL) < creator->last_identify + 5) {
						uint32_t wait = (creator->last_identify + 5) - time(NULL);
						log(dpp::ll_debug, fmt::format("Waiting {} seconds before identifying for session...", wait));
						std::this_thread::sleep_for(std::chrono::seconds(wait));
					}
					log(dpp::ll_debug, "Connecting new session...");
						json obj = {
						{ "op", 2 },
						{
							"d",
							{
								{ "token", this->token },
								{ "properties",
									{
										{ "$os", "Linux" },
										{ "$browser", "D++" },
										{ "$device", "D++" }
									}
								},
								{ "shard", json::array({ shard_id, max_shards }) },
								{ "compress", false },
								{ "large_threshold", 250 }
							}
						}
					};
					if (this->intents) {
						obj["d"]["intents"] = this->intents;
					}
					this->write(obj.dump());
					this->connect_time = creator->last_identify = time(NULL);
					reconnects++;
				}
				this->last_heartbeat_ack = time(nullptr);
			break;
			case 0: {
				std::string event = j.find("t") != j.end() && !j["t"].is_null() ? j["t"] : "";

				HandleEvent(event, j, data);
			}
			break;
			case 7:
				log(dpp::ll_debug, fmt::format("Reconnection requested, closing socket {}", sessionid));
				message_queue.clear();
				::close(sfd);
			break;
			/* Heartbeat ack */
			case 11:
				this->last_heartbeat_ack = time(nullptr);
			break;
		}
	}
	return true;
}

dpp::utility::uptime DiscordClient::Uptime()
{
	return dpp::utility::uptime(time(NULL) - connect_time);
}

bool DiscordClient::IsConnected()
{
	return (this->GetState() == CONNECTED) && (this->ready);
}

void DiscordClient::Error(uint32_t errorcode)
{
	std::map<uint32_t, std::string> errortext = {
		{ 1000, "Socket shutdown" },
		{ 1001, "Client is leaving" },
		{ 1002, "Endpoint received a malformed frame" },
		{ 1003, "Endpoint received an unsupported frame" },
		{ 1004, "Reserved code" },
		{ 1005, "Expected close status, received none" },
		{ 1006, "No close code frame has been receieved" },
		{ 1007, "Endpoint received inconsistent message (e.g. malformed UTF-8)" },
		{ 1008, "Generic error" },
		{ 1009, "Endpoint won't process large frame" },
		{ 1010, "Client wanted an extension which server did not negotiate" },
		{ 1011, "Internal server error while operating" },
		{ 1012, "Server/service is restarting" },
		{ 1013, "Temporary server condition forced blocking client's request" },
		{ 1014, "Server acting as gateway received an invalid response" },
		{ 1015, "Transport Layer Security handshake failure" },
		{ 4000, "Unknown error" },
		{ 4001, "Unknown opcode" },
		{ 4002, "Decode error" },
		{ 4003, "Not authenticated" },
		{ 4004, "Authentication failed" },
		{ 4005, "Already authenticated" },
		{ 4007, "Invalid seq" },
		{ 4008, "Rate limited" },
		{ 4009, "Session timed out" },
		{ 4010, "Invalid shard" },
		{ 4011, "Sharding required" },
		{ 4012, "Invalid API version" },
		{ 4013, "Invalid intent(s)" },
		{ 4014, "Disallowed intent(s)" },
		{ 6000, "ZLib Stream Error" },
		{ 6001, "ZLib Data Error" },
		{ 6002, "ZLib Memory Error" },
		{ 6666, "Hell freezing over" }
	};
	std::string error = "Unknown error";
	auto i = errortext.find(errorcode);
	if (i != errortext.end()) {
		error = i->second;
	}
	log(dpp::ll_warning, fmt::format("OOF! Error from underlying websocket: {}: {}", errorcode, error));
}

void DiscordClient::log(dpp::loglevel severity, const std::string &msg)
{
	if (creator->dispatch.log) {
		/* Pass to user if theyve hooked the event */
		dpp::log_t logmsg(this, msg);
		logmsg.severity = severity;
		logmsg.message = msg;
		creator->dispatch.log(logmsg);
	}
}

void DiscordClient::QueueMessage(const std::string &j, bool to_front)
{
	std::lock_guard<std::mutex> locker(queue_mutex);
	if (to_front) {
		message_queue.push_front(j);
	} else {
		message_queue.push_back(j);
	}
}

void DiscordClient::ClearQueue()
{
	std::lock_guard<std::mutex> locker(queue_mutex);
	message_queue.clear();
}

size_t DiscordClient::GetQueueSize()
{
	std::lock_guard<std::mutex> locker(queue_mutex);
	return message_queue.size();
}

void DiscordClient::OneSecondTimer()
{
	/* This all only triggers if we are connected (have completed websocket, and received READY or RESUMED) */
	if (this->IsConnected()) {

		/* If we stopped getting heartbeat acknowledgements, this means the connections is dead.
		 * This can happen to TCP connections which is why we have heartbeats in the first place.
		 * Miss two ACKS, forces a reconnection.
		 */
		if ((time(nullptr) - this->last_heartbeat_ack) > heartbeat_interval * 2) {
			log(dpp::ll_warning, fmt::format("Missed heartbeat ACK, forcing reconnection to session {}", sessionid));
			message_queue.clear();
			::close(sfd);
			return;
		}

		/* Rate limit outbound messages, 1 every odd second, 2 every even second */
		for (int x = 0; x < (time(NULL) % 2) + 1; ++x) {
			std::lock_guard<std::mutex> locker(queue_mutex);
			if (message_queue.size()) {
				std::string message = message_queue.front();
				message_queue.pop_front();
				this->write(message);
			}
		}

		/* Send pings (heartbeat opcodes) before each interval. We send them slightly more regular than expected,
		 * just to be safe.
		 */
		if (this->heartbeat_interval && this->last_seq) {
			/* Check if we're due to emit a heartbeat */
			if (time(NULL) > last_heartbeat + ((heartbeat_interval / 1000.0) * 0.75)) {
				QueueMessage(json({{"op", 1}, {"d", last_seq}}).dump(), true);
				last_heartbeat = time(NULL);
				dpp::garbage_collection();
			}
		}
	}
}

uint64_t DiscordClient::GetGuildCount() {
	uint64_t total = 0;
	dpp::cache* c = dpp::get_guild_cache();
	dpp::cache_container& gc = c->get_container();
	/* IMPORTANT: We must lock the container to iterate it */
	std::lock_guard<std::mutex> lock(c->get_mutex());
	for (auto g = gc.begin(); g != gc.end(); ++g) {
		dpp::guild* gp = (dpp::guild*)g->second;
		if (gp->shard_id == this->shard_id) {
			total++;
		}
	}
	return total;
}

uint64_t DiscordClient::GetMemberCount() {
	uint64_t total = 0;
	dpp::cache* c = dpp::get_guild_cache();
	dpp::cache_container& gc = c->get_container();
	/* IMPORTANT: We must lock the container to iterate it */
	std::lock_guard<std::mutex> lock(c->get_mutex());
	for (auto g = gc.begin(); g != gc.end(); ++g) {
		dpp::guild* gp = (dpp::guild*)g->second;
		if (gp->shard_id == this->shard_id) {
			total += gp->members.size();
		}
	}
	return total;
}

uint64_t DiscordClient::GetChannelCount() {
	uint64_t total = 0;
	dpp::cache* c = dpp::get_guild_cache();
	dpp::cache_container& gc = c->get_container();
	/* IMPORTANT: We must lock the container to iterate it */
	std::lock_guard<std::mutex> lock(c->get_mutex());
	for (auto g = gc.begin(); g != gc.end(); ++g) {
		dpp::guild* gp = (dpp::guild*)g->second;
		if (gp->shard_id == this->shard_id) {
			total += gp->channels.size();
		}
	}
	return total;
}

void DiscordClient::ConnectVoice(snowflake guild_id, snowflake channel_id) {
#ifdef HAVE_VOICE
	std::lock_guard<std::mutex> lock(voice_mutex);
	if (connecting_voice_channels.find(guild_id) == connecting_voice_channels.end()) {
		connecting_voice_channels[guild_id] = new voiceconn(this, channel_id);
		/* Once sent, this expects two events (in any order) on the websocket:
		* VOICE_SERVER_UPDATE and VOICE_STATUS_UPDATE
		*/
		QueueMessage(json({
			{ "op", 4 },
			{ "d", {
					{ "guild_id", std::to_string(guild_id) },
					{ "channel_id", std::to_string(channel_id) },
					{ "self_mute", false },
					{ "self_deaf", false },
				}
			}
		}).dump(), true);
	}
#endif
}

void DiscordClient::DisconnectVoice(snowflake guild_id) {
#ifdef HAVE_VOICE
	std::lock_guard<std::mutex> lock(voice_mutex);
	auto v = connecting_voice_channels.find(guild_id);
	if (v != connecting_voice_channels.end()) {
		delete v->second;
		connecting_voice_channels.erase(v);
		QueueMessage(json({
			{ "op", 4 },
			{ "d", {
					{ "guild_id", std::to_string(guild_id) },
					{ "channel_id", json::value_t::null },
					{ "self_mute", false },
					{ "self_deaf", false },
				}
			}
		}).dump(), true);
	}
#endif
}

voiceconn* DiscordClient::GetVoice(snowflake guild_id) {
#ifdef HAVE_VOICE
	std::lock_guard<std::mutex> lock(voice_mutex);
	auto v = connecting_voice_channels.find(guild_id);
	if (v != connecting_voice_channels.end()) {
		return v->second;
	}
#endif
	return nullptr;
}


voiceconn::voiceconn(DiscordClient* o, snowflake _channel_id) : creator(o), channel_id(_channel_id), voiceclient(nullptr) {
}

bool voiceconn::is_ready() {
	return (!websocket_hostname.empty() && !session_id.empty() && !token.empty());
}

bool voiceconn::is_active() {
	return voiceclient != nullptr;
}

void voiceconn::disconnect() {
	if (this->is_active()) {
		voiceclient->terminating = true;
		delete voiceclient;
		voiceclient = nullptr;
	}
}

voiceconn::~voiceconn() {
	this->disconnect();
}

void voiceconn::connect(snowflake guild_id) {
	if (this->is_ready()) {
		/* This is wrapped in a thread because instantiating DiscordVoiceClient can initiate a blocking SSL_connect() */
		auto t = std::thread([guild_id, this]() {
			try {
				this->voiceclient = new DiscordVoiceClient(creator->creator, this->channel_id, guild_id, this->token, this->session_id, this->websocket_hostname);
				/* Note: Spawns thread! */
				this->voiceclient->Run();
			}
			catch (std::exception &e) {
				this->creator->log(ll_error, fmt::format("Can't connect to voice websocket (guild_id: {}, channel_id: {}): {}", guild_id, this->channel_id, e.what()));
			}
		});
		t.detach();
	}
}


};