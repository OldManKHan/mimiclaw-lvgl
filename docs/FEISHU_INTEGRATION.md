# Feishu Integration Notes

Date: 2026-02-21

## Implemented capability

Current project now supports a full chain for Feishu self-built app bot:

1. Long connection receive events (`/callback/ws/endpoint` + WebSocket).
2. Decode Feishu protobuf frame (`pbbp2.Frame`) and merge chunked event payload.
3. Push inbound text event into `message_bus` (`channel=feishu`).
4. Send outbound response via `im/v1/messages` (app token auth).

Legacy custom-bot webhook send is still kept as a fallback path.

## Config keys

- NVS namespace: `feishu_cfg`
- Keys:
  - `app_id`
  - `app_secret`
  - `def_chat` (optional default chat_id for test send)
  - `webhook` (legacy fallback)

Build-time fallbacks in `mimi_secrets.h`:

- `MIMI_SECRET_FEISHU_APP_ID`
- `MIMI_SECRET_FEISHU_APP_SECRET`
- `MIMI_SECRET_FEISHU_DEFAULT_CHAT_ID`
- `MIMI_SECRET_FEISHU_WEBHOOK`

## Runtime flow

- `app_main` initializes `feishu_bot`.
- After WiFi is ready, `feishu_bot_start()` starts long-connection task if app creds exist.
- Inbound `im.message.receive_v1` text messages are injected to agent via `message_bus_push_inbound`.
- Agent outbound messages for `MIMI_CHAN_FEISHU` are sent through `im/v1/messages` using inbound chat_id.

## Feishu console setup

- App type: self-built app.
- Enable bot capability and IM message receive/send permissions.
- Event subscription mode: **persistent connection (long connection)**.
- Subscribe event: `im.message.receive_v1`.

## CLI

- `set_feishu_app <app_id> <app_secret>`
- `clear_feishu_app`
- `set_feishu_chat <chat_id>`
- `clear_feishu_chat`
- `feishu_send <chat_id> <text>`

Legacy webhook commands:

- `set_feishu_webhook <url>`
- `clear_feishu_webhook`

## UI

Feishu page now supports:

- App ID
- App Secret
- Default Chat ID
- Save (stores config and starts long connection)
- Test Send
