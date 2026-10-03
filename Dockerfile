FROM python:3.11-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libpqxx-dev libpq-dev libgrpc++-dev \
    libprotobuf-dev protobuf-compiler-grpc postgresql-client \
    && rm -rf /var/lib/apt/lists/*

COPY . /app
RUN python -m pip install --no-cache-dir -r api/requirements.txt -r chat_service/requirements.txt \
    && mkdir -p api/generated \
    && touch api/generated/__init__.py \
    && python -m grpc_tools.protoc -I runtime/proto --python_out=api/generated \
       --grpc_python_out=api/generated runtime/proto/agentos.proto \
    && sed -i 's/^import agentos_pb2 as agentos__pb2$/from . import agentos_pb2 as agentos__pb2/' api/generated/agentos_pb2_grpc.py \
    && cmake -S . -B /tmp/agentos-build -DAGENTOS_ENABLE_POSTGRES=ON -DAGENTOS_ENABLE_GRPC=ON \
    && cmake --build /tmp/agentos-build --target agentos_server -j 2 \
    && cp /tmp/agentos-build/agentos_server /usr/local/bin/agentos_server \
    && rm -rf /tmp/agentos-build

CMD ["bash", "deploy/render-start.sh"]
