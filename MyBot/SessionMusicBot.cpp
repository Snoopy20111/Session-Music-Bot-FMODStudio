#include "SessionMusicBot.h"				//Pre-written sanity checks for versions
#include <dpp/dpp.h>						//D++ header
#include "fmod.hpp"							//FMOD Core
#include "fmod_studio.hpp"					//FMOD Studio
#include "fmod_common.h"
#include "fmod_studio_common.h"
#include "fmod_errors.h"					//Allows FMOD Results to be output as understandable text
#include <filesystem>						//Standard C++ Filesystem
#include <chrono>							//Standard C++ Timekeeping (mostly for debugging?)
#include "SessionMusicBot_Utils.h"			//Some utility functions specific to this bot

/* Be sure to place your token in the line below.
 * Follow steps here to get a token:
 * https://dpp.dev/creating-a-bot-application.html
 * When you invite the bot, be sure to invite it with the 
 * scopes 'bot' and 'applications.commands', e.g.
 * https://discord.com/oauth2/authorize?client_id=940762342495518720&scope=bot+applications.commands&permissions=139586816064
 */

bool isRunning = true;
bool isConnected = false;
bool fromSilence = true;

//file paths
std::filesystem::path exe_path;
std::filesystem::path banks_path;
std::filesystem::path workpath;

//---FMOD Declearations---//
FMOD_RESULT result;										//Reusable error checking result
FMOD::Studio::System* pSystem = nullptr;				//overall system
FMOD::System* pCoreSystem = nullptr;					//overall core system
FMOD::Studio::Bank* pMasterBank = nullptr;				//Master Bank
FMOD::Studio::Bank* pMasterStringsBank = nullptr;		//Master Strings
FMOD::Studio::Bank* pSharedUIBank = nullptr;			//Example non-master bank
FMOD::Studio::Bus* pMasterBus = nullptr;				//Master bus
FMOD::ChannelGroup* pMasterBusGroup = nullptr;			//Channel Group of the master bus
FMOD::DSP* mCaptureDSP = nullptr;						//DSP to attach to Master Channel Group for stealing output

dpp::voiceconn* currentVC = nullptr;					//Goes stale, todo: fix or make it only used temporarily
dpp::discord_voice_client* currentClient = nullptr;

std::vector<uint16_t> myPCMData;						//Main buffer of PCM audio data, which FMOD adds to and D++ cuts "frames" from


//Test Event stuff, will be replaced with more flexible lists built at runtime
FMOD::Studio::EventDescription* pEventDescription = nullptr;        //Event Description, essentially the Event itself plus data
FMOD::Studio::EventInstance* pEventInstance = nullptr;              //Event Instance
FMOD_3D_ATTRIBUTES listenerAttributes;
FMOD_3D_ATTRIBUTES eventAttributes;
std::string master_bank = "Master.bank";							//Known bank names, basically just Master and Strings
std::string masterstrings_bank = "Master.strings.bank";
std::string ui_bank = "Shared_UI.bank";

//FMOD and Audio Functions
void ERRCHECK(FMOD_RESULT result) {
	if (result != FMOD_OK) {
		printf("FMOD Error! (%d) %s\n", result, FMOD_ErrorString(result));
		exit(-1 * result);
	}
}

uint16_t floatToPCM(float inSample) {
	uint16_t outSample;

	if (inSample >= 1.0) { outSample = 32767; }					//Ceiling
	else if (inSample <= -1.0) { outSample = -32768; }			//Floor
	else { outSample = (uint16_t)floor(inSample * 32767.0); }	//Normal conversion

	//Todo: Dithering?

	return outSample;
}

FMOD_RESULT F_CALLBACK captureDSPReadCallback(FMOD_DSP_STATE* dsp_state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int* outchannels) {
	
	FMOD::DSP* thisdsp = (FMOD::DSP*)dsp_state->instance;
	std::vector<uint16_t> pcmdata;

	if (isConnected) {
		for (unsigned int samp = 0; samp < length; samp++) {
			for (int chan = 0; chan < *outchannels; chan++) {
				//outbuffer[(samp * *outchannels) + chan] = inbuffer[(samp * inchannels) + chan];	// This DSP filter just passes out what it got in.
				outbuffer[(samp * *outchannels) + chan] = 0.0f;										//This filter basically just mutes the system output.
				pcmdata.push_back(floatToPCM(inbuffer[(samp*inchannels) + chan]));
			}
		}
	}
	else {
		for (unsigned int samp = 0; samp < length; samp++) {
			for (int chan = 0; chan < *outchannels; chan++) {
				outbuffer[(samp * *outchannels) + chan] = 0.0f;										//This filter basically just mutes the system output.
			}
		}
	}
	//Pass PCM data to our larger buffer
	if (isConnected) {
		myPCMData.insert(myPCMData.end(), pcmdata.cbegin(), pcmdata.cend());
	}

	return FMOD_OK;
}

//Bot Functions
std::string getBotToken()
{
	//read from .config (text) file and grab the token
	std::ifstream myfile ("token.config");
	std::string token;
	if (myfile.is_open())
	{
		myfile >> token;
		std::cout << "Token from config file is: " << token << "\n";
	}
	else
	{
		std::cout << "Token config file not opened properly. Ensure it's at the correct location and isn't corrupted!";
	}
	myfile.close();
	return token;
}

void ping(const dpp::slashcommand_t& event) {
	std::string response = "Pong! I'm alive!";
	event.reply(dpp::message(response.c_str()).set_flags(dpp::m_ephemeral));
	std::cout << response.c_str() << std::endl;
}

void list(const dpp::slashcommand_t& event) {
	std::string response = "List of events (coming soon).";
	event.reply(dpp::message(response.c_str()).set_flags(dpp::m_ephemeral));
	std::cout << response.c_str() << std::endl;
}

void join(const dpp::slashcommand_t& event) {
	dpp::guild* guild = dpp::find_guild(event.command.guild_id);						//Get the Guild aka Server
	currentVC = event.from->get_voice(event.command.guild_id);							//Get the bot's current voice channel
	bool join_vc = true;
	if (currentVC) {																	//If already in voice...
		auto users_vc = guild->voice_members.find(event.command.get_issuing_user().id);	//get channel of user
		if ((users_vc != guild->voice_members.end()) && (currentVC->channel_id == users_vc->second.channel_id)) {
			join_vc = false;			//skips joining a voice chat below
		}
		else {
			event.from->disconnect_voice(event.command.guild_id);						//We're in a different VC, so leave it and join the new one below
			join_vc = true;																//possibly redundant assignment?
		}
	}
	if (join_vc) {																		//If we need to join a call above...
		if (!guild->connect_member_voice(event.command.get_issuing_user().id)) {		//try to connect, return false if we fail
			event.reply(dpp::message("You're not in a voice channel to be joined!").set_flags(dpp::m_ephemeral));
			std::cout << "Not in a voice channel to be joined." << std::endl;
			return;
		}
		//If not caught above, we're in voice! Not instant, will need to wait for on_voice_ready callback
		event.reply(dpp::message("Joined your channel!").set_flags(dpp::m_ephemeral));
		std::cout << "Joined channel of user." << std::endl;

	}
	else {
		event.reply(dpp::message("I am already living in your walls.").set_flags(dpp::m_ephemeral));
		std::cout << "Already living in your walls." << std::endl;
	}
}

void leave(const dpp::slashcommand_t& event) {
	currentVC = event.from->get_voice(event.command.guild_id);
	if (currentVC) {
		event.from->disconnect_voice(event.command.guild_id);
		event.reply(dpp::message("Bye bye! I hope I played good sounds!").set_flags(dpp::m_ephemeral));
		std::cout << "Leaving voice channel." << std::endl;
		isConnected = false;
	}
}

//void indexBanks()
//void indexEvents()????
//void listEvents()
//void play()
//void updateParam()
//void stop()


void init() {
	std::cout << "###########################" << std::endl;
	std::cout << "###                     ###" << std::endl;
	std::cout << "###  Session Music Bot  ###" << std::endl;
	std::cout << "###                     ###" << std::endl;
	std::cout << "###########################" << std::endl;

	//file paths
	exe_path = getExecutableFolder();				//Special function from SessionMusicBot_Utils.h
	banks_path = exe_path.append("soundbanks");
	workpath = banks_path;
	workpath.append(master_bank);					//Sets workpath to default file, to ensure there's always at least a valid path

	//FMOD Init
	std::cout << "Initializing FMOD...";
	ERRCHECK(FMOD::Studio::System::create(&pSystem));
	ERRCHECK(pSystem->initialize(128, FMOD_STUDIO_INIT_NORMAL, FMOD_INIT_NORMAL, nullptr));
	ERRCHECK(pSystem->getCoreSystem(&pCoreSystem));
	std::cout << "Done." << std::endl;


	//Load Master Bank and Master Strings
	std::cout << "Loading banks...";
	ERRCHECK(pSystem->loadBankFile(workpath.replace_filename(master_bank).string().c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &pMasterBank));
	ERRCHECK(pSystem->loadBankFile(workpath.replace_filename(masterstrings_bank).string().c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &pMasterStringsBank));
	ERRCHECK(pSystem->loadBankFile(workpath.replace_filename(ui_bank).string().c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &pSharedUIBank));
	std::cout << "Done." << std::endl;

	//Also get the Master Bus, set volume, and get the related Channel Group
	std::cout << "Getting Busses and Channel Groups...";
	ERRCHECK(pSystem->getBus("bus:/", &pMasterBus));
	ERRCHECK(pMasterBus->setVolume(dBToFloat(-10.0f)));
	ERRCHECK(pMasterBus->lockChannelGroup());					//Tell the Master Channel Group to always exist even when events arn't playing...
	ERRCHECK(pSystem->flushCommands());							//And wait until all previous commands are done (ensuring Channel Group exists)...
	ERRCHECK(pMasterBus->getChannelGroup(&pMasterBusGroup));	//Or else this fails immediately, and we'll have DSP problems.
	std::cout << "Done." << std::endl;


	//Define and create our capture DSP on the Master Channel Group.
	//Copied from FMOD's examples, unsure why this works and why it must be in brackets.
	{
		FMOD_DSP_DESCRIPTION dspdesc;
		memset(&dspdesc, 0, sizeof(dspdesc));
		strncpy_s(dspdesc.name, "LH_captureDSP", sizeof(dspdesc.name));
		dspdesc.version = 0x00010000;
		dspdesc.numinputbuffers = 2;
		dspdesc.numoutputbuffers = 2;
		dspdesc.read = captureDSPReadCallback;
		//dspdesc.userdata = (void*)0x12345678;
		ERRCHECK(pCoreSystem->createDSP(&dspdesc, &mCaptureDSP));
	}
	ERRCHECK(pMasterBusGroup->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, mCaptureDSP));		//Adds the newly defined dsp


	//Setting Listener positioning for 3D, if desired. Normally done from Game Engine data
	std::cout << "Setting up Listener...";
	listenerAttributes.position = { 0.0f, 0.0f, 0.0f };
	listenerAttributes.forward = { 0.0f, 1.0f, 0.0f };
	listenerAttributes.up = { 0.0f, 0.0f, 1.0f };
	ERRCHECK(pSystem->setListenerAttributes(0, &listenerAttributes));
	std::cout << "Done." << std::endl;

	//Create event instance
	std::cout << "Creating Test Event Instance...";
	//ERRCHECK(pSystem->getEvent("event:/Master/Sine", &pEventDescription));
	ERRCHECK(pSystem->getEvent("event:/Master/Music/TitleTheme", &pEventDescription));
	ERRCHECK(pEventDescription->createInstance(&pEventInstance));
	std::cout << "Done." << std::endl;

	//Debug details
	int samplerate; FMOD_SPEAKERMODE speakermode; int numrawspeakers;
	ERRCHECK(pCoreSystem->getSoftwareFormat(&samplerate, &speakermode, &numrawspeakers));
	ERRCHECK(pSystem->flushCommands());
	std::cout << "###########################" << std::endl;
	std::cout << "FMOD System Info:\n  Sample Rate- " << samplerate << "\n  Speaker Mode- " << speakermode
		<< "\n  Num Raw Speakers- " << numrawspeakers << std::endl;


	std::cout << "###########################" << std::endl;
	std::cout << "###                     ###" << std::endl;
	std::cout << "###   All systems go!   ###" << std::endl;
	std::cout << "###                     ###" << std::endl;
	std::cout << "###########################" << std::endl;
}

int main() {

	init();

	auto start = std::chrono::system_clock::now();
	auto end = std::chrono::system_clock::now();
	auto last = end;
	std::chrono::duration<double, std::milli> elapsed;
	std::chrono::duration<double, std::milli> elapsed_since_last;


	/* Create bot cluster */
	dpp::cluster bot(getBotToken());

	/* Output simple log messages to stdout */
	bot.on_log(dpp::utility::cout_logger());

	/* Register slash command here in on_ready */
	bot.on_ready([&bot](const dpp::ready_t& event) {
		/* Wrap command registration in run_once to make sure it doesnt run on every full reconnection */
		if (dpp::run_once<struct register_bot_commands>()) {
			std::vector<dpp::slashcommand> commands {
				{ "ping", "Ping the bot to ensure it's alive.", bot.me.id },
				{ "list", "List all found banks/events.", bot.me.id},
				{ "join", "Join your current voice channel.", bot.me.id},
				{ "leave", "Leave the current voice channel.", bot.me.id}
			};
			bot.global_bulk_command_create(commands);
		}
	});

	/* Handle slash commands */
	bot.on_slashcommand([&bot](const dpp::slashcommand_t& event) {
		if (event.command.get_command_name() == "ping") { ping(event); }
		else if (event.command.get_command_name() == "list") { list(event); }
		else if (event.command.get_command_name() == "join") { join(event); }
		else if (event.command.get_command_name() == "leave") { leave(event); }
	});

	bot.on_voice_ready([&bot](const dpp::voice_ready_t& event) {
		std::cout << "Voice Ready\n";
		currentClient = event.voice_client;							//Get the bot's current voice channel
		isConnected = true;
		ERRCHECK(pEventInstance->start());
		ERRCHECK(pEventInstance->release());
	});

	/* Start the bot */
	bot.start();


	//FMOD update loop here?
	while (isRunning) {

		//Update time
		last = end;
		end = std::chrono::system_clock::now();
		elapsed = end - start;
		elapsed_since_last = end - last;

		pSystem->update();
		
		//Sending PCM Data to D++ for encoding and encrypting
		if (isConnected) {
			//The first time playing from silence, send a bunch of packets to build some time.
			//This will add some latency from "play" to transmission, but necessary
			// to avoid starving D++ of samples
			if (fromSilence && (myPCMData.size() > dpp::send_audio_raw_max_length * 50)) {					// 69120 samples, ~ 1.5 sec
				std::cout << "Sending PCM Data from silence at time: " << elapsed << std::endl;

				while (myPCMData.size() > dpp::send_audio_raw_max_length * 2) {								//Until minimum size we want our buffer
					std::cout << "[] ";
					std::vector<uint16_t> pcmdata = myPCMData;												//Copy the buffer
					pcmdata.resize(dpp::send_audio_raw_max_length);											//trim copy down to one "frame"
					currentClient->send_audio_raw(pcmdata.data(), pcmdata.size());							//Send over the data
					myPCMData.erase(myPCMData.begin(), myPCMData.begin() + dpp::send_audio_raw_max_length);	//Trim our main buffer of the data just sent
				}
				fromSilence = false;
			}
			//Standard loop
			else if (!fromSilence && (myPCMData.size() > dpp::send_audio_raw_max_length * 2)) {
				std::cout << "Sending PCM Data at time: " << elapsed << std::endl;
				std::vector<uint16_t> pcmdata = myPCMData;
				pcmdata.resize(dpp::send_audio_raw_max_length);					//cuts out everything we don't want
				currentClient->send_audio_raw(pcmdata.data(), pcmdata.size());
				myPCMData.erase(myPCMData.begin(), myPCMData.begin() + dpp::send_audio_raw_max_length);
			}
			//Todo: what if the audio ends? We should stop transmitting, right? Probably would go here.
			//Check if any events are playing, and if not plus output is silent, fromSilence = true again.
		} 
		Sleep(10);
	}
	std::cout << "Quitting program. Releasing resources..." << std::endl;

	//When closing, remove DSP from master channel group, and release the DSP
	pMasterBusGroup->removeDSP(mCaptureDSP);
	mCaptureDSP->release();						//Is this one necessary, given below?

	//Unload and release System
	pSystem->unloadAll();
	pSystem->release();

	return 0;
}