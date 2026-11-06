FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY app.py /app/app.py
COPY templates /app/templates
COPY static /app/static

RUN useradd -m -u 1000 appuser && mkdir -p /data && chown -R appuser:appuser /data /app
USER appuser

ENV PORT=8080 \
    DATA_DIR=/data \
    PUBLIC_URL=http://localhost:8080

VOLUME ["/data"]
EXPOSE 8080

CMD ["python3", "/app/app.py"]
