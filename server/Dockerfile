FROM python:3.12-slim

WORKDIR /app
COPY app.py /app/app.py

RUN useradd -m -u 1000 appuser && mkdir -p /data && chown -R appuser:appuser /data /app
USER appuser

ENV PORT=8080 \
    DATA_DIR=/data \
    PUBLIC_URL=http://localhost:8080 \
    UPLOAD_TOKEN=change-me

VOLUME ["/data"]
EXPOSE 8080

CMD ["python3", "/app/app.py"]
