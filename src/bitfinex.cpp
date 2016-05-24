#include <string.h>
#include <iostream>
#include <unistd.h>
#include <sstream>
#include <math.h>
#include <sys/time.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include "base64.h"
#include <jansson.h>
#include "bitfinex.h"
#include "curl_fun.h"

namespace Bitfinex
{

	double getQuote(Parameters& params, bool isBid)
	{
		bool GETRequest = true;
		json_t* root = getJsonFromUrl(params, "https://api.bitfinex.com/v1/ticker/btcusd", "", GETRequest);
		const char* quote;
		double quoteValue;
		if (isBid)
		{
			quote = json_string_value(json_object_get(root, "bid"));
		}
		else
		{
			quote = json_string_value(json_object_get(root, "ask"));
		}
		if (quote != NULL)
		{
			quoteValue = atof(quote);
		}
		else
		{
			quoteValue = 0.0;
		}
		json_decref(root);
		return quoteValue;
	}

	double getAvail(Parameters& params, std::string currency)
	{
		json_t* root = authRequest(params, "https://api.bitfinex.com/v1/balances", "balances", "");
		while (json_object_get(root, "message") != NULL)
		{
			sleep(1.0);
			*params.logFile << "<Bitfinex> Error with JSON: " << json_dumps(root, 0) << ". Retrying..." << std::endl;
			root = authRequest(params, "https://api.bitfinex.com/v1/balances", "balances", "");
		}
		size_t arraySize = json_array_size(root);
		double availability = 0.0;
		for (size_t i = 0; i < arraySize; i++)
		{
			std::string tmpType = json_string_value(json_object_get(json_array_get(root, i), "type"));
			std::string tmpCurrency = json_string_value(json_object_get(json_array_get(root, i), "currency"));
			if (tmpType.compare("trading") == 0 && tmpCurrency.compare(currency.c_str()) == 0)
			{
				availability = atof(json_string_value(json_object_get(json_array_get(root, i), "amount")));
			}
		}
		json_decref(root);
		return availability;
	}

	int sendLongOrder(Parameters& params, std::string direction, double quantity, double price)
	{
		return sendOrder(params, direction, quantity, price);
	}

	int sendShortOrder(Parameters& params, std::string direction, double quantity, double price)
	{
		return sendOrder(params, direction, quantity, price);
	}

	int sendOrder(Parameters& params, std::string direction, double quantity, double price)
	{
		*params.logFile << "<Bitfinex> Trying to send a \"" << direction << "\" limit order: " << quantity << "@$" << price << "..." << std::endl;
		std::ostringstream oss;
		oss << "\"symbol\":\"btcusd\", \"amount\":\"" << quantity << "\", \"price\":\"" << price << "\", \"exchange\":\"bitfinex\", \"side\":\"" << direction << "\", \"type\":\"limit\"";
		std::string options = oss.str();
		json_t* root = authRequest(params, "https://api.bitfinex.com/v1/order/new", "order/new", options);
		int orderId = json_integer_value(json_object_get(root, "order_id"));
		*params.logFile << "<Bitfinex> Done (order ID: " << orderId << ")\n" << std::endl;
		json_decref(root);
		return orderId;
	}

	bool isOrderComplete(Parameters& params, int orderId)
	{
		if (orderId == 0)
		{
			return true;
		}
		std::ostringstream oss;
		oss << "\"order_id\":" << orderId;
		std::string options = oss.str();
		json_t* root = authRequest(params, "https://api.bitfinex.com/v1/order/status", "order/status", options);
		bool isComplete = !json_boolean_value(json_object_get(root, "is_live"));
		json_decref(root);
		return isComplete;
	}

	double getActivePos(Parameters& params)
	{
		json_t* root = authRequest(params, "https://api.bitfinex.com/v1/positions", "positions", "");
		double position;
		if (json_array_size(root) == 0)
		{
			*params.logFile << "<Bitfinex> WARNING: BTC position not available, return 0.0" << std::endl;
			position = 0.0;
		}
		else
		{
			position = atof(json_string_value(json_object_get(json_array_get(root, 0), "amount")));
		}
		json_decref(root);
		return position;
	}

	double getLimitPrice(Parameters& params, double volume, bool isBid)
	{
		json_t* root;
		bool GETRequest = true;
		if (isBid)
		{
			root = json_object_get(getJsonFromUrl(params, "https://api.bitfinex.com/v1/book/btcusd", "", GETRequest), "bids");
		}
		else
		{
			root = json_object_get(getJsonFromUrl(params, "https://api.bitfinex.com/v1/book/btcusd", "", GETRequest), "asks");
		}
		*params.logFile << "<Bitfinex> Looking for a limit price to fill " << fabs(volume) << " BTC..." << std::endl;
		double tmpVol = 0.0;
		double p;
		double v;
		int i = 0;
		// loop on volume
		while (tmpVol < fabs(volume) * params.orderBookFactor)
		{
			p = atof(json_string_value(json_object_get(json_array_get(root, i), "price")));
			v = atof(json_string_value(json_object_get(json_array_get(root, i), "amount")));
			*params.logFile << "<Bitfinex> order book: " << v << "@$" << p << std::endl;
			tmpVol += v;
			i++;
		}
		double limPrice = 0.0;
		limPrice = atof(json_string_value(json_object_get(json_array_get(root, i - 1), "price")));
		json_decref(root);
		return limPrice;
	}

	json_t* authRequest(Parameters& params, std::string url, std::string request, std::string options)
	{
		struct timeval tv;
		gettimeofday(&tv, NULL);
		unsigned long long nonce = (tv.tv_sec * 1000.0) + (tv.tv_usec * 0.001) + 0.5;
		std::ostringstream oss;
		if (options.empty())
		{
			oss << "{\"request\":\"/v1/" << request << "\",\"nonce\":\"" << nonce << "\"}";
		}
		else
		{
			oss << "{\"request\":\"/v1/" << request << "\",\"nonce\":\"" << nonce << "\", " << options << "}";
		}
		std::string tmpPayload = base64_encode(reinterpret_cast<const unsigned char*>(oss.str().c_str()), oss.str().length());
		oss.clear();
		oss.str("");
		oss << "X-BFX-PAYLOAD:" << tmpPayload;
		std::string payload;
		payload = oss.str();
		oss.clear();
		oss.str("");
		// signature
		unsigned char* digest;
		digest = HMAC(EVP_sha384(), params.bitfinexSecret.c_str(), strlen(params.bitfinexSecret.c_str()), (unsigned char*)tmpPayload.c_str(), strlen(tmpPayload.c_str()), NULL, NULL);
		char mdString[SHA384_DIGEST_LENGTH + 100];   // FIXME +100
		for (int i = 0; i < SHA384_DIGEST_LENGTH; ++i)
		{
			sprintf(&mdString[i * 2], "%02x", (unsigned int)digest[i]);
		}
		oss.clear();
		oss.str("");
		oss << "X-BFX-SIGNATURE:" << mdString;
		struct curl_slist *headers = NULL;
		std::string api = "X-BFX-APIKEY:" + std::string(params.bitfinexApi);
		headers = curl_slist_append(headers, api.c_str());
		headers = curl_slist_append(headers, payload.c_str());
		headers = curl_slist_append(headers, oss.str().c_str());
		CURLcode resCurl;
		if (params.curl)
		{
			std::string readBuffer;
			curl_easy_setopt(params.curl, CURLOPT_POST, 1L);
			curl_easy_setopt(params.curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(params.curl, CURLOPT_POSTFIELDS, "");
			curl_easy_setopt(params.curl, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(params.curl, CURLOPT_WRITEFUNCTION, WriteCallback);
			curl_easy_setopt(params.curl, CURLOPT_WRITEDATA, &readBuffer);
			curl_easy_setopt(params.curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(params.curl, CURLOPT_CONNECTTIMEOUT, 10L);
			resCurl = curl_easy_perform(params.curl);
			json_t* root;
			json_error_t error;
			while (resCurl != CURLE_OK)
			{
				*params.logFile << "<Bitfinex> Error with cURL. Retry in 2 sec..." << std::endl;
				sleep(2.0);
				readBuffer = "";
				resCurl = curl_easy_perform(params.curl);
			}
			root = json_loads(readBuffer.c_str(), 0, &error);
			while (!root)
			{
				*params.logFile << "<Bitfinex> Error with JSON:\n" << error.text << std::endl;
				*params.logFile << "<Bitfinex> Buffer:\n" << readBuffer.c_str() << std::endl;
				*params.logFile << "<Bitfinex> Retrying..." << std::endl;
				sleep(2.0);
				readBuffer = "";
				resCurl = curl_easy_perform(params.curl);
				while (resCurl != CURLE_OK)
				{
					*params.logFile << "<Bitfinex> Error with cURL. Retry in 2 sec..." << std::endl;
					sleep(2.0);
					readBuffer = "";
					resCurl = curl_easy_perform(params.curl);
				}
				root = json_loads(readBuffer.c_str(), 0, &error);
			}
			curl_slist_free_all(headers);
			curl_easy_reset(params.curl);
			return root;
		}
		else
		{
			*params.logFile << "<Bitfinex> Error with cURL init." << std::endl;
			return NULL;
		}
	}

}
