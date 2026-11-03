extern "C" int network_fixture_value();

extern "C" int network_fixture_backend_value() {
    return network_fixture_value();
}
