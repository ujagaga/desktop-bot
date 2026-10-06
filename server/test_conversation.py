"""Gateway integration checks against local fake Gemini and recognition servers."""
import asyncio
import base64
import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

import numpy as np

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
            await ws.send_bytes(b'{"setupComplete": {}}')  # binary, like real Gemini
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
            'recognize_url': str(self.fake.make_url('/recognize')), 'voice': ''}
        self.voice_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.voice_dir.cleanup)
        self.voice_file = os.path.join(self.voice_dir.name, 'voice.json')
        with open(os.path.join(self.voice_dir.name, 'hello.py'), 'w') as f:
            f.write("DESCRIPTION = 'Greeting'\ndef run():\n    return 'Hi there'\n")
        with open(os.path.join(self.voice_dir.name, 'show.py'), 'w') as f:
            f.write("DESCRIPTION = 'Shows'\ndef run():\n    return {'say': '14:32', 'clock': '14:32', 'seconds': 5, 'sleep': True}\n")
        with open(os.path.join(self.voice_dir.name, 'broken.py'), 'w') as f:
            f.write("DESCRIPTION = 'Fails'\ndef run():\n    raise OSError('secret')\n")
        self.app = conversation.create_app(self.config, str(self.fake.make_url('/live')), self.voice_file,
                                           self.voice_dir.name)
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
        self.assertEqual(ready['output_sample_rate'], 16000)
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
        self.assertNotIn('speechConfig', self.setup['setup']['generationConfig'])
        instruction = self.setup['setup']['systemInstruction']['parts'][0]['text']
        self.assertTrue(instruction.startswith('Test instructions Current local date and time'))
        await ws.send_bytes(b'\x01\x00' * 320)
        data = await asyncio.wait_for(self.messages.get(), 2)
        self.assertEqual(base64.b64decode(data['realtimeInput']['audio']['data']), b'\x01\x00' * 320)
        await ws.send_json({'type': 'audio_end'})
        self.assertEqual(await asyncio.wait_for(self.messages.get(), 2), {'realtimeInput': {'audioStreamEnd': True}})
        await ws.send_json({'type': 'text', 'text': 'Hello'})
        self.assertTrue((await asyncio.wait_for(self.messages.get(), 2))['clientContent']['turnComplete'])
        await self.provider.send_json({'serverContent': {
            'modelTurn': {'parts': [{'inlineData': {'mimeType': 'audio/pcm;rate=24000', 'data': 'AAAAAAAA'}}]},
            'outputTranscription': {'text': 'Hello!'}, 'turnComplete': True}})
        self.assertEqual((await ws.receive(timeout=2)).data, b'\x00' * 4)
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'transcript')
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'turn_complete')
        big = base64.b64encode(b'\x00' * 30000).decode()  # 20000 bytes after 3:2 resampling
        await self.provider.send_json({'serverContent': {'modelTurn': {'parts': [
            {'inlineData': {'mimeType': 'audio/pcm;rate=24000', 'data': big}}]}}})
        frames = [len((await ws.receive(timeout=2)).data) for _ in range(5)]
        self.assertEqual(frames, [4096] * 4 + [20000 - 4 * 4096])
        await self.provider.send_json({'serverContent': {'interrupted': True}})
        self.assertEqual((await ws.receive_json(timeout=2))['type'], 'interrupted')
        await ws.close()

    async def test_configured_voice_is_sent(self):
        self.config['voice'] = 'Kore'
        ws = await self.connect()
        voice = self.setup['setup']['generationConfig']['speechConfig']['voiceConfig']['prebuiltVoiceConfig']
        self.assertEqual(voice['voiceName'], 'Kore')
        await ws.close()

    async def test_voice_endpoint_lists_validates_and_persists(self):
        response = await self.client.get('/voice')
        self.assertEqual(response.status, 401)
        listed = await (await self.client.get('/voice', headers=self.headers)).json()
        self.assertEqual(listed['voice'], '')
        self.assertIn('Kore', listed['voices'])
        response = await self.client.post('/voice', json={'voice': 'Bogus'}, headers=self.headers)
        self.assertEqual(response.status, 400)
        response = await self.client.post('/voice', json={'voice': 'Puck'}, headers=self.headers)
        self.assertEqual((await response.json())['voice'], 'Puck')
        ws = await self.connect()
        voice = self.setup['setup']['generationConfig']['speechConfig']['voiceConfig']['prebuiltVoiceConfig']
        self.assertEqual(voice['voiceName'], 'Puck')
        await ws.close()
        reloaded = conversation.create_app(dict(self.config, voice=''), voice_file=self.voice_file)
        self.assertEqual(reloaded[conversation.CONFIG]['voice'], 'Puck')

    async def test_commands_are_declared_and_run(self):
        ws = await self.connect()
        declared = self.setup['setup']['tools'][0]['functionDeclarations']
        self.assertEqual([d['name'] for d in declared], ['people_present', 'broken', 'hello', 'show'])
        await self.provider.send_json({'toolCall': {'functionCalls': [
            {'id': '1', 'name': 'hello', 'args': {}}, {'id': '2', 'name': 'broken'}, {'id': '3', 'name': 'nope'}]}})
        responses = (await asyncio.wait_for(self.messages.get(), 2))['toolResponse']['functionResponses']
        self.assertEqual(responses, [
            {'id': '1', 'name': 'hello', 'response': {'result': 'Hi there'}},
            {'id': '2', 'name': 'broken', 'response': {'error': 'Command failed.'}},
            {'id': '3', 'name': 'nope', 'response': {'error': 'Unknown command.'}}])
        await self.provider.send_json({'toolCall': {'functionCalls': [{'id': '4', 'name': 'show'}]}})
        self.assertEqual(await ws.receive_json(timeout=2), {'type': 'display', 'clock': '14:32', 'seconds': 5})
        self.assertEqual(await ws.receive_json(timeout=2), {'type': 'sleep'})
        responses = (await asyncio.wait_for(self.messages.get(), 2))['toolResponse']['functionResponses']
        self.assertEqual(responses[0]['response'], {'result': '14:32'})
        await ws.close()

    async def test_snapshot_is_recognized_locally(self):
        ws = await self.connect()
        await ws.send_json({'type': 'snapshot', 'jpeg': base64.b64encode(b'jpeg').decode()})
        self.assertEqual((await ws.receive_json(timeout=2))['names'], ['Alice'])
        # Nothing is pushed to Gemini; it asks with the people_present tool instead.
        await self.provider.send_json({'toolCall': {'functionCalls': [{'id': '1', 'name': 'people_present'}]}})
        reply = (await asyncio.wait_for(self.messages.get(), 2))['toolResponse']['functionResponses'][0]
        self.assertIn('Alice', reply['response']['result'])
        await ws.close()

    async def test_snapshot_post_reaches_session(self):
        ws = await self.client.ws_connect('/conversation', headers=self.headers)
        session = (await ws.receive_json(timeout=2))['session']
        response = await self.client.post('/snapshot?session=' + session, data=b'jpeg')
        self.assertEqual(response.status, 401)
        response = await self.client.post('/snapshot?session=wrong', data=b'jpeg', headers=self.headers)
        self.assertEqual(response.status, 404)
        response = await self.client.post('/snapshot?session=' + session, data=b'jpeg', headers=self.headers)
        self.assertEqual((await response.json())['names'], ['Alice'])
        self.assertEqual((await ws.receive_json(timeout=2))['names'], ['Alice'])
        self.assertTrue(self.messages.empty())
        await ws.close()
        await asyncio.sleep(0.1)
        self.assertEqual(self.app[conversation.SNAPSHOT_TARGETS], {})

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


class SetupMessageTests(unittest.TestCase):
    def test_clock_is_appended_to_instructions(self):
        now = datetime(2026, 10, 5, 23, 14, tzinfo=timezone(timedelta(hours=2)))
        text = conversation.setup_message({'model': 'm', 'instructions': 'Hi.'}, now)['setup']['systemInstruction']['parts'][0]['text']
        self.assertEqual(text, 'Hi. Current local date and time at the start of this conversation: '
                               'Monday, 05 October 2026, 23:14 (UTC+0200).')


class DownsamplerTests(unittest.TestCase):
    def test_chunked_output_matches_one_shot_and_keeps_pitch(self):
        tone = (8000 * np.sin(2 * np.pi * 1000 * np.arange(2400) / 24000)).astype('<i2').tobytes()
        whole = conversation.Downsampler().process(tone)
        chunked = conversation.Downsampler()
        parts = b''.join(chunked.process(tone[i:i + n]) for i, n in zip(range(0, 4800, 902), [902] * 6))
        self.assertEqual(len(whole), 1600 * 2)
        self.assertEqual(parts, whole)
        out = np.frombuffer(whole, '<i2')[100:].astype(np.float32)
        spectrum = np.abs(np.fft.rfft(out))
        self.assertAlmostEqual(np.argmax(spectrum) * 16000 / len(out), 1000, delta=20)


if __name__ == '__main__':
    unittest.main()
