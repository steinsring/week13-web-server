FROM gcc:14-bookworm@sha256:5e927c284bf55a7dc796262e311a0703344f62f41f5621eb56843111b1d37e15
WORKDIR /app
COPY Makefile *.c *.h ./
COPY start.sh ./
COPY tests/ ./tests/
COPY public/ ./public/
RUN make && chmod +x start.sh
EXPOSE 8080
CMD ["./server", "8080", "/app/public", "4", "64", "100", "1048576", "30", "10", "100", "http://localhost:8080", "5", "10", "http://127.0.0.1:8001,http://127.0.0.1:8002"]
