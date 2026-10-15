extern "C" int network_fixture_backend_value();

int main() {
    return network_fixture_backend_value() == FIXTURE_VALUE ? 0 : 1;
}
