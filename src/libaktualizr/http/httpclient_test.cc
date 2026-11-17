#include <gtest/gtest.h>

#include "http/httpclient.h"

#include <atomic>
#include <boost/process.hpp>
#include <chrono>
#include <string>
#include <thread>

#include "json/json.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "utilities/flow_control.h"
#include "utilities/utils.h"

// NOLINTNEXTLINE(*non-const*)
static std::string server = "http://127.0.0.1:";

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, CopyConstructorTest) {
  HttpClient http;
  HttpClient http_copy(http);
  std::string path = "/path/1/2/3";
  Json::Value resp = http_copy.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(resp["path"].asString(), path);
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Get) {
  HttpClient http;
  std::string path = "/path/1/2/3";
  Json::Value response = http.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(response["path"].asString(), path);
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, GetWithHeaders) {
  std::vector<std::string> headers = {"Authorization: Bearer token"};
  HttpClient http(&headers);
  std::string path = "/auth_call";
  Json::Value response = http.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(response["status"].asString(), "good");
}

/* Reject http GET responses that exceed size limit. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, DownloadSizeLimit) {
  HttpClient http;
  std::string path = "/large_file";
  HttpResponse resp = http.get(server + path, 1024, nullptr);
  std::cout << "RESP SIZE " << resp.body.length() << std::endl;
  EXPECT_EQ(resp.curl_code, CURLE_FILESIZE_EXCEEDED);
}

/* Reject http GET responses that do not meet speed limit. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, DownloadSpeedLimit) {
  HttpClient http;
  std::string path = "/slow_file";

  http.overrideSpeedLimitParams(3, 5000);
  HttpResponse resp = http.get(server + path, HttpInterface::kNoLimit, nullptr);
  EXPECT_EQ(resp.curl_code, CURLE_OPERATION_TIMEDOUT);
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Cancellation) {
  HttpClient http;
  std::string path = "/slow_file";
  api::FlowControlToken token;
  std::atomic<bool> did_abort{false};
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::thread abort_thread([&token, end, &did_abort] {
    std::this_thread::sleep_until(end);
    token.setAbort();
    did_abort = true;
  });
  HttpResponse resp = http.get(server + path, HttpInterface::kNoLimit, &token);
  auto actual_end = std::chrono::steady_clock::now();
  EXPECT_TRUE(did_abort);
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(actual_end - end).count();

  LOG_INFO << "Took:" << diff << "ms to abort";
  // Curl takes ~2 seconds to call the progress meter and abort
  EXPECT_LE(0, diff);
  EXPECT_LE(diff, 3000);
  EXPECT_EQ(resp.curl_code, CURLE_ABORTED_BY_CALLBACK);
  abort_thread.join();
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Post) {
  HttpClient http;
  std::string path = "/path/1/2/3";
  Json::Value data;
  data["key"] = "val";

  Json::Value response = http.post(server + path, data).getJson();
  EXPECT_EQ(response["path"].asString(), path);
  EXPECT_EQ(response["data"]["key"].asString(), "val");
  // Too small for gzip to pay off, so it goes out uncompressed.
  EXPECT_EQ(response["encoding"].asString(), "");
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Put) {
  HttpClient http;
  std::string path = "/path/1/2/3";
  Json::Value data;
  data["key"] = "val";

  Json::Value json = http.put(server + path, data).getJson();

  EXPECT_EQ(json["path"].asString(), path);
  EXPECT_EQ(json["data"]["key"].asString(), "val");
}

static Json::Value compressibleBody() {
  Json::Value data;
  data["key"] = std::string(4096, 'x');
  return data;
}

/* Request bodies that shrink under gzip are sent with Content-Encoding: gzip. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, PostPutGzip) {
  HttpClient http;
  const Json::Value data = compressibleBody();

  Json::Value response = http.post(server + "/path", data).getJson();
  EXPECT_EQ(response["encoding"].asString(), "gzip");
  EXPECT_EQ(response["data"], data);

  response = http.put(server + "/path", data).getJson();
  EXPECT_EQ(response["encoding"].asString(), "gzip");
  EXPECT_EQ(response["data"], data);
}

/* A 415 to a gzipped body triggers an uncompressed resend, and the client
 * stops compressing from then on. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, PostGzipFallbackOn415) {
  HttpClient http;
  const Json::Value data = compressibleBody();

  HttpResponse resp = http.post(server + "/reject_gzip", data);
  ASSERT_EQ(resp.http_status_code, 200);
  Json::Value response = resp.getJson();
  EXPECT_EQ(response["encoding"].asString(), "");
  EXPECT_EQ(response["data"], data);

  response = http.post(server + "/path", data).getJson();
  EXPECT_EQ(response["encoding"].asString(), "");
  EXPECT_EQ(response["data"], data);

  // Copies keep the fallback, fresh clients do not.
  HttpClient http_copy(http);
  EXPECT_EQ(http_copy.post(server + "/path", data).getJson()["encoding"].asString(), "");
  HttpClient http_fresh;
  EXPECT_EQ(http_fresh.post(server + "/path", data).getJson()["encoding"].asString(), "gzip");
}

/* Responses may be gzip-encoded; curl decodes them transparently. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, GetAcceptsGzip) {
  HttpClient http;
  HttpResponse resp = http.get(server + "/gzip_response", HttpInterface::kNoLimit, nullptr);
  ASSERT_TRUE(resp.isOk());
  EXPECT_NE(resp.getJson()["accept_encoding"].asString().find("gzip"), std::string::npos);
}

static size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userp) {
  static_cast<std::string*>(userp)->append(ptr, size * nmemb);
  return size * nmemb;
}

/* Downloads must stay byte-exact, so they don't ask for an encoding. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, DownloadNoAcceptEncoding) {
  HttpClient http;
  std::string body;
  HttpResponse resp = http.download(server + "/gzip_response", appendToString, nullptr, &body, 0);
  ASSERT_TRUE(resp.isOk());
  EXPECT_EQ(Utils::parseJSON(body)["accept_encoding"].asString(), "");
}

/* An in-flight download keeps the shared curl state alive after its client
 * has been destroyed. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, DownloadOutlivesClient) {
  const std::string url = server + "/slow_file";
  std::string body;
  std::future<HttpResponse> resp;
  {
    HttpClient http;
    resp = http.downloadAsync(url, appendToString, nullptr, &body, 0);
  }
  EXPECT_TRUE(resp.get().isOk());
  EXPECT_EQ(body, std::string(20, 'a'));
}

static std::string tlsHandshakeKind(const HttpResponse& resp) {
  // openssl s_server -www reports each connection as "New, <protocol>, ..."
  // or "Reused, <protocol>, ...".
  if (resp.body.find("\nReused, ") != std::string::npos) {
    return "Reused";
  }
  if (resp.body.find("\nNew, ") != std::string::npos) {
    return "New";
  }
  return "unknown: " + resp.body;
}

/* Every request opens a new connection, but it resumes the TLS session of an
 * earlier one instead of repeating the full handshake. Copies of a client
 * share its sessions. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, TlsSessionResumption) {
  const std::string port = TestUtils::getFreePort();
  boost::process::child tls_server(boost::process::search_path("openssl"), "s_server", "-accept", port, "-www", "-cert",
                                   "tests/fake_http_server/server.crt", "-key", "tests/fake_http_server/server.key",
                                   boost::process::std_out > boost::process::null);
  const std::string url = "https://localhost:" + port + "/";
  const std::string cert = Utils::readFile("tests/fake_http_server/server.crt");
  const std::string key = Utils::readFile("tests/fake_http_server/server.key");

  // Wait for the server with a separate client, so the client under test
  // starts with an empty session cache.
  {
    HttpClient probe;
    probe.setCerts(cert, CryptoSource::kFile, cert, CryptoSource::kFile, key, CryptoSource::kFile);
    for (int i = 0; i < 50 && !probe.get(url, HttpInterface::kNoLimit, nullptr).isOk(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  HttpClient http;
  http.setCerts(cert, CryptoSource::kFile, cert, CryptoSource::kFile, key, CryptoSource::kFile);
  EXPECT_EQ(tlsHandshakeKind(http.get(url, HttpInterface::kNoLimit, nullptr)), "New");
  EXPECT_EQ(tlsHandshakeKind(http.get(url, HttpInterface::kNoLimit, nullptr)), "Reused");

  HttpClient http_copy(http);
  EXPECT_EQ(tlsHandshakeKind(http_copy.get(url, HttpInterface::kNoLimit, nullptr)), "Reused");

  tls_server.terminate();
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, UserAgent) {
  {
    // test the default, when setUserAgent hasn't been called yet
    HttpClient http;

    const auto resp = http.get(server + "/user_agent", HttpInterface::kNoLimit, nullptr);
    const auto app = resp.body.substr(0, resp.body.find('/'));
    EXPECT_EQ(app, "Aktualizr");
  }

  Utils::setUserAgent("blah");

  {
    HttpClient http;

    auto resp = http.get(server + "/user_agent", HttpInterface::kNoLimit, nullptr);
    EXPECT_EQ(resp.body, "blah");
  }
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, UpdateHeader) {
  std::vector<std::string> headers = {"Authorization: Bearer bad"};
  HttpClient http(&headers);

  ASSERT_FALSE(http.updateHeader("NOSUCHHEADER", "foo"));

  std::string path = "/auth_call";
  std::string body = http.get(server + path, HttpInterface::kNoLimit, nullptr).body;
  EXPECT_EQ(body, "{}");

  ASSERT_TRUE(http.updateHeader("Authorization", "Bearer token"));
  Json::Value response = http.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(response["status"].asString(), "good");
}

/* Verify that concurrent post() calls from multiple threads do not crash or
 * corrupt data.  The fake server is started with -f (intermittent 503s), so
 * we only assert that the curl call itself succeeded (no transport error) and
 * that every thread ran to completion. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, ConcurrentPosts) {
  HttpClient http;

  std::vector<std::thread> threads;
  threads.reserve(4);
  std::vector<bool> results(4, false);
  for (int i = 0; i < 4; i++) {
    threads.emplace_back([&http, &results, i]() {
      for (int j = 0; j < 10; j++) {
        Json::Value data;
        data["thread"] = i;
        data["iteration"] = j;
        auto resp = http.post(server + "/events", data);
        EXPECT_EQ(resp.curl_code, CURLE_OK);
      }
      results[static_cast<size_t>(i)] = true;
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  for (int i = 0; i < 4; i++) {
    EXPECT_TRUE(results[static_cast<size_t>(i)]);
  }
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  std::string port = TestUtils::getFreePort();
  server += port;
  boost::process::child server_process("tests/fake_http_server/fake_test_server.py", port, "-f");
  TestUtils::waitForServer(server + "/");

  return RUN_ALL_TESTS();
}
#endif
