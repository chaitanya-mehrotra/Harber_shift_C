FROM python:3.12-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libcurl4-openssl-dev uuid-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY backend ./backend
RUN gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
    backend/*.c backend/vendor/cJSON.c backend/vendor/yxml.c -o harborshift -lcurl -luuid -lm

FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libcurl4 libuuid1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements-model.txt .
RUN pip install --no-cache-dir -r requirements-model.txt
COPY --from=build /build/harborshift ./harborshift-server
COPY frontend/dist ./frontend/dist
COPY data ./data
COPY artifacts ./artifacts
COPY harborshift ./harborshift
ENV PORT=10000 HARBORSHIFT_BIND_ALL=1 OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
EXPOSE 10000
USER 10001:10001
CMD ["./harborshift-server"]
