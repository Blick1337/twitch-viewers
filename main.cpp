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
#include <unistd.h>
using json = nlohmann::json;

typedef size_t(*CURL_WRITEFUNCTION_PTR)(void*, size_t, size_t, void*);
std::vector<std::string> proxyarr;
std::vector<std::string> useragentarr;
std::string username;

const int threadTimeout = 5000;

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

std::string sendRequest(CURL *curl, struct curl_slist *headers, std::string proxy, std::string addr)
{
	std::string content;
	long response_code;
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, addr.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<CURL_WRITEFUNCTION_PTR>(write_to_string));
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);
	if(!proxy.empty())
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
	
	auto res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

	if (res || response_code != 200)
		return std::string();
	return content;
}

void threadLoop(std::string proxy, std::string useragent, std::function<void()> callback)
{
	curl_global_init(CURL_GLOBAL_ALL);
	CURL *curl = curl_easy_init();

	while (true)
	{
		__try
		{
			struct curl_slist *headers = NULL;
			headers = curl_slist_append(headers, std::string("Referer: https://www.twitch.tv/" + username).c_str());
			headers = curl_slist_append(headers, "Client-ID: jzkbprff40iqj646a697cyrvl0zt2m6");
			headers = curl_slist_append(headers, std::string("User-Agent: " + useragent).c_str());

			auto ret = sendRequest(curl, headers, proxy, "https://api.twitch.tv/api/channels/" + username + "/access_token?need_https=true&oauth_token&platform=web&player_backend=mediaplayer&player_type=site");

			if (ret.empty())
				break;

			json val = json::parse(ret);

			std::string token = val["token"].get<std::string>();
			std::string sig = val["sig"].get<std::string>();
			std::srand(unsigned(std::time(0)));
			int randVal = 1 + rand() % 999999;


			struct curl_slist *m3uheaders = NULL;
			m3uheaders = curl_slist_append(m3uheaders, "Accept: application/x-mpegURL, application/vnd.apple.mpegurl, application/json, text/plain");
			m3uheaders = curl_slist_append(m3uheaders, std::string("User-Agent: " + useragent).c_str());

			auto ret2 = sendRequest(curl, m3uheaders, proxy, "https://usher.ttvnw.net/api/channel/hls/" + username + ".m3u8?player=twitchweb&token=" + url_encode(token) + "&sig=" + sig + "&allow_audio_only=true&allow_source=true&type=any&p=" + std::to_string(randVal));

			if (ret2.empty())
				break;

			struct curl_slist *tsHeader = NULL;
			tsHeader = curl_slist_append(tsHeader, "Accept: application/x-mpegURL, application/vnd.apple.mpegurl, application/json, text/plain");
			tsHeader = curl_slist_append(tsHeader, std::string("User-Agent: " + useragent).c_str());

			std::string requestAddr = parseString("https://", ".m3u8", ret2);

			auto ret3 = sendRequest(curl, tsHeader, proxy, "https://" + requestAddr + ".m3u8");

			if (ret3.empty())
				break;

			printf("[INFO] Proxy %s connected\n", proxy.c_str());

			sleep(threadTimeout / 1000);
		}
		catch (...)
		{
			printf("[EXCEPTION] Proxy %s\n", proxy.c_str());
		}
	}

	curl_easy_cleanup(curl);
	curl_global_cleanup();

	callback();
}

void eraseProxy(std::string proxy)
{
	for (size_t i = 0; i < proxyarr.size(); i++)
	{
		auto elem = proxyarr.at(i);
		if (elem == proxy)
			proxyarr.erase(proxyarr.begin() + i);
	}
};

void startThread()
{
	if (proxyarr.size() <= 0)
	{
		printf("[ERROR] Proxy List Ended\n");
		return;
	}

	auto it = useragentarr.begin();
	std::advance(it, rand() % useragentarr.size());
	std::thread tr(threadLoop, proxyarr[0], *it, startThread);
	eraseProxy(proxyarr[0]);
	tr.detach();
};

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		printf("[ERROR] Invalid Arguments. Example %s <username> <threads count>\n", argv[0]);
		return 0;
	}

	username = argv[1];
	int threadCount = atoi(argv[2]);

	{
		std::ifstream in("./proxies.txt", std::ios::in);

		if (in.is_open())
		{
			std::string line;
			while (getline(in, line))
				proxyarr.push_back(line);
			in.close();

			printf("[INFO] Loaded %i proxies\n", proxyarr.size());
		}
		else
		{
			printf("[ERROR] Proxies not found\n");
			return 0;
		}
	}

	{
		std::ifstream in("./user-agents.txt", std::ios::in);

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

	printf("[INFO] Starting for %s with %i thread's\n", username.c_str(), threadCount);

	for (int i = 0; i < threadCount; i++)
		startThread();

	printf("[INFO] Started\n");

	getchar();
    return 0;
}
