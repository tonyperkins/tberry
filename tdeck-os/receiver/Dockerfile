FROM python:3.12-slim
WORKDIR /app
COPY server.py .
ENV COV_DATA_DIR=/data COV_PORT=8765
VOLUME /data
EXPOSE 8765
USER nobody
CMD ["python", "server.py"]
