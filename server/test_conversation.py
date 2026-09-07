"""Gateway integration checks against local fake Gemini and recognition servers."""
import asyncio
import base64
import unittest

from aiohttp import web, WSMsgType
from aiohttp.test_utils import TestClient, TestServer
import conversation


class ConversationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.messages = asyncio.Queue()
        self.provider = None
        self.reject_setup = False
        async def gemini(request):
            self.assertEqual(request.headers['x-goog-api-key'], 'cloud-secret')
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            self.provider = ws
            self.setup = await ws.receive_json()
            if self.reject_setup:
                await ws.send_json({'error': {'message': 'secret cloud-secret provider detail'}})
                await ws.close()
                return ws
            await ws.send_json({'setupComplete': {}})
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    import json
                    await self.messages.put(json.loads(msg.data))
            return ws
        async def recognize(request):
            self.assertEqual(request.headers['X-API-Key'], 'device-secret')
            self.assertEqual(await request.read(), b'jpeg')
            return web.json_response({'names': ['Alice'], 'faces': [{'name': 'Alice'}]})
        fake = web.Application()
        fake.router.add_get('/live', gemini)
        fake.router.add_post('/recognize', recognize)
        self.fake = TestServer(fake)
        await self.fake.start_server()
        self.config = {'api_key': 'device-secret', 'gemini_key': 'cloud-secret',
            'model': 'test-model', 'instructions': 'Test instructions', 'max_sessions': 1,
            'recognize_url': str(self.fake.make_url('/recognize'))}
        self.app = conversation.create_app(self.config, str(self.fake.make_url('/live')))
        self.client = TestClient(TestServer(self.app))
        await self.client.start_server()
        self.headers = {'X-API-Key': 'device-secret'}

    async def asyncTearDown(self):
        await self.client.close()
        await self.fake.close()

    async def connect(self):
        ws = await self.client.ws_connect('/conversation', headers=self.headers)
        ready = await ws.receive_json(timeout=2)
        self.assertEqual(ready['type'], 'ready')
        self.assertEqual(ready['output_sample_rate'], 24000)
        return ws

    async def test_auth_and_missing_configuration(self):
        for path in ['/health', '/conversation']:
            response = await self.client.get(path)
            self.assertEqual(response.status, 401)
        self.config['gemini_key'] = ''
        response = await self.client.get('/conversation', headers=self.headers)
        self.assertEqual(response.status, 503)
        response = await self.client.get('/health', headers=self.headers)
        self.assertFalse((await response.json())['gemini_configured'])

    async def test_audio_text_and_provider_events(self):
        ws = await self.connect()
        self.assertEqual(self.setup['setup']['model'], 'models/test-model')
        await ws.send_bytes(b'\x01\x00' * 320)
        data = await asyncio.wait_for(self.messages.get(), 2)
        self.assertEqual(base64.b64decode(data['realtimeInput']['audio']['data']), b'\x01\x00' * 320)
        await ws.send_json({'type': 'audio_end'})
        self.assertEqual(await asyncio.wait_for(self.messages.get(), 2), {'realtimeInput': {'audioStreamEnd': True}})
        await ws.send_json({'type': 'text', 'text': 'Hello'})
        self.assertTrue((await asyncio.wait_for(self.messages.get(), 2))['clientContent']['turnComplete'])
        await self.provider.send_json({'serverContent': {
            'modelTurn': {'parts': [{'inlineData': {'mimeType': 'audio/pcm;rate=24000', 'data': 'AAA='}}]},
            'outputTranscription': {'text': 'Hello!'}, 'turnComplete': True}})
        self.assertEqual((await ws.receive(timeout=2)).data, b'\x00\x00')
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'transcript')
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'turn_complete')
        await self.provider.send_json({'serverContent': {'interrupted': True}})
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'interrupted')
        await ws.close()

    async def test_snapshot_is_recognized_locally(self):
        ws = await self.connect()
        await ws.send_json({'type': 'snapshot', 'jpeg': base64.b64encode(b'jpeg').decode()})
        self.assertEqual((await ws.receive_json(timeout=2))['names'], ['Alice'])
        upstream = await asyncio.wait_for(self.messages.get(), 2)
        self.assertIn('Alice', upstream['realtimeInput']['text'])
        self.assertNotIn('video', upstream['realtimeInput'])
        await ws.close()

    async def test_invalid_messages_capacity_and_stop(self):
        ws = await self.connect()
        response = await self.client.get('/conversation', headers=self.headers)
        self.assertEqual(response.status, 429)
        for message in ['not json', '[]', '{"type":"snapshot","jpeg":"!"}', '{"type":"text","text":""}']:
            await ws.send_str(message)
            self.assertEqual((await ws.receive_json(timeout=2))['type'], 'error')
        await ws.send_bytes(b'odd')
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'error')
        await ws.send_json({'type': 'stop'})
        self.assertEqual((await ws.receive(timeout=2)).type, WSMsgType.CLOSE)

    async def test_provider_failure_is_redacted_and_capacity_released(self):
        self.reject_setup = True
        ws = await self.client.ws_connect('/conversation', headers=self.headers)
        event = await ws.receive_json(timeout=2)
        self.assertEqual(event['type'], 'error')
        self.assertNotIn('cloud-secret', str(event))
        self.assertEqual((await ws.receive(timeout=2)).type, WSMsgType.CLOSE)
        self.assertEqual(len(self.app[conversation.SESSIONS]), 0)

    async def test_upgrade_required(self):
        response = await self.client.get('/conversation', headers=self.headers)
        self.assertEqual(response.status, 400)

    async def test_provider_shutdown_notifies_client(self):
        ws = await self.connect()
        await self.provider.send_json({'goAway': {'timeLeft': '30s'}})
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'session_ending')
        self.assertEqual((await ws.receive(timeout=2)).type, WSMsgType.CLOSE)


if __name__ == '__main__':
    unittest.main()
