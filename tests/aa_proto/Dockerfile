# mmi-aa-proto - Google's protobuf for test_android_auto (tests/check_aa_proto.py):
# protoc (grpcio-tools) compiles aasdk's .proto files, the protobuf runtime
# encodes the phone's messages and decodes ours. Versions and hashes are
# locked in requirements.txt (uv pip compile --generate-hashes, from
# requirements.in).
FROM ghcr.io/astral-sh/uv:0.5.11-python3.12-bookworm-slim

COPY requirements.txt /opt/aa-proto/requirements.txt
RUN uv pip install --system --require-hashes -r /opt/aa-proto/requirements.txt
