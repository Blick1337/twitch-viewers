#include <cstdio>
#include <string>
#include <iostream>
#include "curl/curl.h"
#include <vector>
#include <fstream>
#include <ostream>
#include <istream>
#include <functional>
#include <sstream>
#include <ctime>
#include <iomanip>
#include "json.h"
#include <thread>
#include <mutex>
#include <future>
#include <random>
#include <deque>
#include <unistd.h>

std::mutex g_mutex;
std::mutex g_mutex_proxy;

using json = nlohmann::json;

typedef size_t(*CURL_WRITEFUNCTION_PTR)(void*, size_t, size_t, void*);
std::vector<std::string> proxyarr;
std::vector<std::string> useragentarr;
std::vector<std::thread::id> threads;
std::string username;
int threadCount;

const int threadTimeout = 2000;

std::string parseString(std::string before, std::string after, std::string source)
{
	if (!before.empty() && !after.empty() && !source.empty() && (source.find(before) != std::string::npos) && (source.find(after) != std::string::npos))
	{
		std::string t = strstr(source.c_str(), before.c_str());
		t.erase(0, before.length());
		std::string::size_type loc = t.find(after, 0);
		t = t.substr(0, loc);
		return t;
	}
	else
		return "";
}

size_t write_to_string(void *ptr, size_t size, size_t count, void *stream) {
	((std::string*)stream)->append((char*)ptr, 0, size*count);
	return size * count;
}

std::string url_encode(const std::string &value) {
	std::string new_str = "";
	char c;
	int ic;
	const char* chars = value.c_str();
	char bufHex[10];
	int len = (int)strlen(chars);

	for (int i = 0; i < len; i++) {
		c = chars[i];
		ic = c;
		// uncomment this if you want to encode spaces with +
		/*if (c==' ') new_str += '+';
		else */if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':' || c == ',') new_str += c;
		else {
			sprintf(bufHex, "%X", c);
			if (ic < 16)
				new_str += "%0";
			else
				new_str += "%";
			new_str += bufHex;
		}
	}
	return new_str;
}

std::string sendRequest(std::string addr, std::string proxy, std::vector<std::string> headers, bool isConnectionOnly = false, bool isResponseNeeded = true)
{
	__try
	{
		CURL *curl = curl_easy_init();

		struct curl_slist *header = NULL;

		for (auto elem : headers)
			header = curl_slist_append(header, elem.c_str());

		std::string content;
		long response_code;
		curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);
		curl_easy_setopt(curl, CURLOPT_URL, addr.c_str());
		
		if (!isResponseNeeded)
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		else
			curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);

		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<CURL_WRITEFUNCTION_PTR>(write_to_string));
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);

		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);


		curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1);
		curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1);


		if (!proxy.empty())
		{
			curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
			//curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
		}

		if (isConnectionOnly)
			curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1);

		auto res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

		curl_easy_cleanup(curl);
		ZeroMemory(header, sizeof(header));
		header = NULL;

		if (isConnectionOnly)
		{
			if (res == CURLE_OK)
				return "nobody";
			else return "";
		}

		if (res != CURLE_OK || response_code != 200)
			return "";

		return content;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return "";
	}
}

void threadLoop(std::string proxy, std::string useragent)
{
	while (!g_mutex.try_lock()) usleep(100000);
	threads.push_back(std::this_thread::get_id()); // push currently threadid in array
	g_mutex.unlock();

	while (true)
	{
		//send request on api twitch for hls api
		auto api_request = sendRequest("https://api.twitch.tv/api/channels/" + username + "/access_token?need_https=true&oauth_token&platform=web&player_backend=mediaplayer&player_type=site", proxy,
			{ "Referer: https://www.twitch.tv/" + username, "Client-ID: jzkbprff40iqj646a697cyrvl0zt2m6", "User-Agent: " + useragent });

		if (api_request.empty())
			break;

		json val = json::parse(api_request);

		//parse token and sigs
		std::string token = val["token"].get<std::string>();
		std::string sig = val["sig"].get<std::string>();
		
		usleep(100000);

		//send request on hls api for get m3u8 playlist
		auto hls_request = sendRequest("https://usher.ttvnw.net/api/channel/hls/" + username + ".m3u8?allow_source=true&baking_bread=true&baking_brownies=true&baking_brownies_timeout=1050&fast_bread=true&p=3168255&player_backend=mediaplayer&playlist_include_framerate=true&reassignments_supported=false&rtqos=business_logic_reverse&cdm=wv&sig=" + sig + "&token=" + url_encode(token), proxy,
			{ "Accept: application/x-mpegURL, application/vnd.apple.mpegurl, application/json, text/plain", "User-Agent: " + useragent });

		if (hls_request.empty())
			break;

		//parse first m3u8 playlist 
		std::string requestAddr = parseString("https://", ".m3u8", hls_request);

		usleep(100000);

		//send 9 request on m3u8 highest resolution, head request == nobody response
		for (int i = 0; i < 9; i++)
		{
			sendRequest("https://" + requestAddr + ".m3u8", proxy, { "Accept: application/x-mpegURL, application/vnd.apple.mpegurl, application/json, text/plain", "User-Agent: " + useragent }, false, true);
			
			//sleep after send request
			sleep(threadTimeout/1000);
		}

		usleep(500000);
	}

	while (!g_mutex.try_lock()) usleep(100000);
	auto it = std::find(threads.begin(), threads.end(), std::this_thread::get_id());

	if (it != threads.end())
		threads.erase(it); // erase currently thread in array 

	g_mutex.unlock();

	printf("[INFO] Proxy %s is down\n", proxy.c_str());
}

void checkProxyLoop()
{
	//get random useragent
	auto it = useragentarr.begin()
	std::advance(it, rand() % useragentarr.size());

	//if proxyarrays is ended - stop thread
	while (!proxyarr.empty())
	{
		while (threads.size() > threadCount) usleep(5000000);

		while (!g_mutex_proxy.try_lock()) usleep(100000);
		std::string proxy = proxyarr.front(); // get first element in array
		proxyarr.erase(proxyarr.begin()); // erase first element
		g_mutex_proxy.unlock();

		//std::string title = "Threads Active: " + std::to_string(threads.size()) + " Proxy left: " + std::to_string(proxyarr.size());
		//SetConsoleTitleA(title.c_str());

		//send request(connection only) for check proxy on valid
		auto request = sendRequest("https://api.twitch.tv/kraken/games/top", proxy, { "Accept: application/vnd.twitchtv.v5+json", "Client-ID: jzkbprff40iqj646a697cyrvl0zt2m6", "User-Agent: " + *it }, true);

		//if request is not empty - create thread
		if (!request.empty())
			std::thread(threadLoop, proxy, *it).detach();

		usleep(500000);
	}
}

int main(int argc, char* argv[])
{

	if (argc < 2)
	{
		printf("[ERROR] Invalid Arguments. Example %s <username> <threads count>\n", argv[0]);
		return 0;
	}

	username = argv[1];
	threadCount = atoi(argv[2]);

	{
		std::ifstream in("../proxies.txt", std::ios::in);

		if (in.is_open())
		{
			g_mutex.lock();
			std::string line;
			while (getline(in, line))
				proxyarr.push_back(line);
			in.close();
			g_mutex.unlock();

			printf("[INFO] Loaded %i proxies\n", proxyarr.size());
		}
		else
		{
			printf("[ERROR] Proxies not found\n");
			return 0;
		}
	}

	{
		std::ifstream in("../user-agents.txt", std::ios::in);

		if (in.is_open())
		{
			std::string line;
			while (getline(in, line))
				useragentarr.push_back(line);
			in.close();

			printf("[INFO] Loaded %i User-Agent's\n", useragentarr.size());
		}
		else
		{
			printf("[ERROR] User-Agent's list not found\n");
			return 0;
		}
	}

	curl_global_init(CURL_GLOBAL_ALL);

	printf("[INFO] Starting thread's for checking proxy\n");

	for (int i = 0; i < 5; i++)
		std::thread(checkProxyLoop).detach();

	printf("[INFO] Starting for %s with %i thread's\n", username.c_str(), threadCount);
	

	getchar();

	curl_global_cleanup();
	return 0;
}
