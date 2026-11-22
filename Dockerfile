FROM gcc:14-bookworm@sha256:5e927c284bf55a7dc796262e311a0703344f62f41f5621eb56843111b1d37e15
WORKDIR /app
COPY Makefile *.c *.h ./
COPY tests/ ./tests/
COPY public/ ./public/
RUN make
EXPOSE 8080
CMD ["./server", "8080", "/app/public", "4", "64", "100"]
