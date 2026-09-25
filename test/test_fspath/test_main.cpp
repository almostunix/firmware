// Unit tests for src/FSPath.h: the path checks that keep XModem, the file manifest and the web
// server away from files they must not serve, overwrite or delete.
#include "Arduino.h"
#include "FSPath.h"
#include "TestUtil.h"
#include <string>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void assertCanonical(const char *expected, const char *path)
{
    TEST_ASSERT_EQUAL_STRING(expected, canonicalFsPath(path).c_str());
}

// --- canonicalFsPath ---

void test_root_and_empty_are_root()
{
    assertCanonical("/", "/");
    assertCanonical("/", "");
}

void test_plain_path_unchanged()
{
    assertCanonical("/static/index.html", "/static/index.html");
}

void test_repeated_slashes_collapse()
{
    assertCanonical("/prefs/tailscale.conf", "//prefs//tailscale.conf");
}

void test_dot_segments_removed()
{
    assertCanonical("/static/a", "/static/./a");
}

void test_dotdot_climbs_out_of_a_directory()
{
    assertCanonical("/prefs/x", "/static/../prefs/x");
    assertCanonical("/prefs/tailscale_keys.json", "/static/x/../../prefs/tailscale_keys.json");
}

void test_dotdot_stops_at_root()
{
    assertCanonical("/etc", "/../../etc");
    assertCanonical("/", "..");
}

void test_relative_and_trailing_slash_normalised()
{
    assertCanonical("/static/a", "static/a");
    assertCanonical("/static", "/static/");
}

void test_max_len_bounds_the_read()
{
    TEST_ASSERT_EQUAL_STRING("/prefs", canonicalFsPath("/prefs/tailscale.conf", 7).c_str());
}

void test_stops_at_embedded_nul()
{
    const char buf[] = "/static\0/../prefs";
    TEST_ASSERT_EQUAL_STRING("/static", canonicalFsPath(buf, sizeof(buf)).c_str());
}

// --- fsPathIsWithin ---

void test_within_accepts_the_directory_and_its_children()
{
    TEST_ASSERT_TRUE(fsPathIsWithin("/static", "/static"));
    TEST_ASSERT_TRUE(fsPathIsWithin("/static/a/b", "/static"));
}

void test_within_rejects_prefix_lookalikes_and_outsiders()
{
    TEST_ASSERT_FALSE(fsPathIsWithin("/staticfoo", "/static"));
    TEST_ASSERT_FALSE(fsPathIsWithin("/prefs/x", "/static"));
    TEST_ASSERT_FALSE(fsPathIsWithin("/", "/static"));
}

void test_within_after_canonicalisation_catches_traversal()
{
    TEST_ASSERT_FALSE(fsPathIsWithin(canonicalFsPath("/static/../prefs/config.proto"), "/static"));
}

// --- isTailscaleSecretPath ---

void test_secret_files_detected()
{
    TEST_ASSERT_TRUE(isTailscaleSecretPath("/prefs/tailscale.conf"));
    TEST_ASSERT_TRUE(isTailscaleSecretPath("/prefs/tailscale_keys.json"));
}

void test_other_files_not_secret()
{
    TEST_ASSERT_FALSE(isTailscaleSecretPath("/prefs/config.proto"));
    TEST_ASSERT_FALSE(isTailscaleSecretPath("/static/prefs/tailscale.conf"));
    // littlefs is case-sensitive, so this names a different (non-secret) file.
    TEST_ASSERT_FALSE(isTailscaleSecretPath("/PREFS/tailscale.conf"));
}

void test_secret_detected_through_disguises()
{
    TEST_ASSERT_TRUE(isTailscaleSecretPath(canonicalFsPath("/static/../prefs/./tailscale.conf")));
    TEST_ASSERT_TRUE(isTailscaleSecretPath(canonicalFsPath("//prefs//tailscale_keys.json")));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_root_and_empty_are_root);
    RUN_TEST(test_plain_path_unchanged);
    RUN_TEST(test_repeated_slashes_collapse);
    RUN_TEST(test_dot_segments_removed);
    RUN_TEST(test_dotdot_climbs_out_of_a_directory);
    RUN_TEST(test_dotdot_stops_at_root);
    RUN_TEST(test_relative_and_trailing_slash_normalised);
    RUN_TEST(test_max_len_bounds_the_read);
    RUN_TEST(test_stops_at_embedded_nul);
    RUN_TEST(test_within_accepts_the_directory_and_its_children);
    RUN_TEST(test_within_rejects_prefix_lookalikes_and_outsiders);
    RUN_TEST(test_within_after_canonicalisation_catches_traversal);
    RUN_TEST(test_secret_files_detected);
    RUN_TEST(test_other_files_not_secret);
    RUN_TEST(test_secret_detected_through_disguises);
    exit(UNITY_END());
}

void loop() {}
