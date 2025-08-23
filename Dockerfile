# Minimal production Dockerfile for t2d_server prototype
# Uses multi-stage to keep final image small.

FROM alpine:3.19 AS runtime
# Expected to copy in pre-built binary via CI artifact (static or with musl-compatible deps)
WORKDIR /app
# Placeholder; CI will replace with actual artifact extraction
COPY build/t2d_server /app/t2d_server
COPY config /app/config
# Non-root user for security
RUN adduser -D -u 10001 t2d && chown -R t2d /app
USER t2d
EXPOSE 40000
ENTRYPOINT ["/app/t2d_server", "config/server.yaml"]
