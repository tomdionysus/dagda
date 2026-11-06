//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What Macha's own loop needs of a node while it stays Macha's (until stage 8c):
// its adverts as the port expects them.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <span>
#include <string>

#include "dagda/domain/wrap.h"
#include "dagda/node.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda {
namespace {

using test::Cluster;
using test::ClusterNode;

// Custody and no certificate, as a Macha node under its cluster key has: the
// advert is signed by a certificate's key, so there is none to publish, and the
// node opens without it. The cluster fixture always issues a certificate, so this
// builds its Dannan node by hand.
TEST(NodeSeams, ANodeWithoutACertificateOpensAndAdvertisesNoWrapKey) {
  dannan::test::TempDir dir;
  test::ClusterIo io;
  std::ofstream(dir.path() / "cluster.key") << "seams test";
  const auto source = dannan::identity::KeySource::typed_passphrase(
      [] {
        const std::string phrase = "node";
        return dannan::identity::Secret(std::span(reinterpret_cast<const uint8_t*>(phrase.data()), phrase.size()));
      },
      dannan::identity::Argon2Costs{1024, 1, 1});
  const auto dannan_state = dir.path() / "node";
  (void)dannan::identity::KeyCustody::create(dannan_state, source);
  dannan::NodeConfig config;
  config.state_path = dannan_state;
  config.key = dannan::identity::load_cluster_key(dir.path() / "cluster.key");
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;
  config.version = "dagda-test";
  config.custody = source;
  config.log = std::make_shared<test::RecordingSink>();
  dannan::Node host(io.context, config);
  ASSERT_FALSE(host.context().identity().certificate().has_value());

  NodeOptions options;
  options.state = dir.path() / "state";
  options.key = test::known_answer_key();
  options.pool.backends.push_back({dir.path() / "data", 64ULL * 1024 * 1024, 0});
  options.maintenance.automatic = false;
  Node node(host.context(), options);
  EXPECT_TRUE(node.running());
  const auto self = host.context().nodes().self();
  EXPECT_FALSE(self.advert(domain::wrap_advert).has_value());
  EXPECT_EQ(domain::verify_wrap_advert(self, std::nullopt, 0).why, "no wrap advert");
  // The key itself is custody's, and is still there for what wraps with it.
  EXPECT_TRUE(std::ranges::equal(node.wrap_key().public_key(), domain::WrapKey::derive(host.context().identity())->public_key()));
  // The store's own adverts are as they are on any node.
  EXPECT_TRUE(self.advert(store::extents_advert).has_value());
  node.stop();
}

// With a certificate the node still publishes it, and it verifies.
TEST(NodeSeams, ANodeWithACertificateStillAdvertisesItsWrapKey) {
  Cluster cluster;
  auto& node = cluster.add("node");
  cluster.start_all();
  const auto now = node.context().time().unix_ms();
  const auto checked = domain::verify_wrap_advert(node.context().nodes().self(), cluster.ca().certificate(), now);
  EXPECT_TRUE(checked.ok) << checked.why;
}

}  // namespace
}  // namespace dagda
