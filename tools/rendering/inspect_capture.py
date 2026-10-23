"""Run with qrenderdoc --python tools/rendering/inspect_capture.py.

Read reports/rendering/request.json to select a capture and optional events.
This creates diagnostic artifacts and exits the diagnostic process.
"""
import json
import os
from pathlib import Path
import traceback
import struct
import renderdoc as rd

root = Path.cwd()
request = json.loads((root / 'reports/rendering/request.json').read_text())
out = root / 'reports/rendering' / request.get('output', 'inspection')
out.mkdir(parents=True, exist_ok=True)
capture = root / request['capture']


def numeric(resource):
    return int(str(resource).rsplit('::', 1)[-1])


def fields(value, depth=0):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if depth > 6:
        return str(value)
    if isinstance(value, (list, tuple)):
        return [fields(x, depth + 1) for x in value]
    if isinstance(value, rd.ResourceId):
        return str(value)
    if isinstance(value, bytes):
        return value.hex()
    if hasattr(value, '__len__') and hasattr(value, '__getitem__'):
        return [fields(x, depth + 1) for x in value]
    result = {}
    for name in dir(value):
        if name.startswith('_') or name in ('this', 'thisown'):
            continue
        item = getattr(value, name)
        if not callable(item):
            result[name] = fields(item, depth + 1)
    return result or str(value)


def run():
    cap = rd.OpenCaptureFile()
    status = cap.OpenFile(str(capture), '', None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError(str(status))
    thumbnail = cap.GetThumbnail(rd.FileType.PNG, 0)
    if thumbnail.data:
        (out / 'captured-thumbnail.png').write_bytes(bytes(thumbnail.data))
    (out / 'renderdoc-log-path.txt').write_text(rd.GetLogFile())
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError(str(status))
    try:
        result = {'capture': str(capture), 'textures': [], 'events': [], 'actions': []}
        def visit(actions):
            for action in actions:
                result['actions'].append({'event': action.eventId, 'name': action.GetName(controller.GetStructuredFile()),
                    'indices': action.numIndices, 'outputs': [numeric(x) for x in action.outputs],
                    'source': numeric(action.copySource), 'destination': numeric(action.copyDestination)})
                visit(action.children)
        visit(controller.GetRootActions())
        textures = controller.GetTextures()
        for texture in textures:
            result['textures'].append({'id': numeric(texture.resourceId), 'width': texture.width,
                'height': texture.height, 'format': texture.format.Name()})
        result['buffers'] = [{'id': numeric(x.resourceId), 'size': x.length} for x in controller.GetBuffers()]
        # Write the inventory even if a subsequent detailed request fails.
        (out / 'inventory.json').write_text(json.dumps(result, indent=2))
        events = request.get('events') or [result['actions'][-1]['event']]
        texture_ids = request.get('textures')
        for event in events:
            controller.SetFrameEvent(event, True)
            pipeline = controller.GetPipelineState()
            detail = {'event': event, 'stages': []}
            if request.get('debug_strip_float_controls'):
                reflection = pipeline.GetShaderReflection(rd.ShaderStage.Pixel)
                raw = bytes(reflection.rawBytes)
                words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
                patched = list(words[:5])
                offset = 5
                while offset < len(words):
                    count, opcode = words[offset] >> 16, words[offset] & 65535
                    inst = words[offset:offset + count]
                    if not ((opcode == 17 and inst[1] in (4465, 4467)) or
                            (opcode == 16 and inst[2] in (4460, 4462))):
                        patched.extend(inst)
                    offset += count
                binary = struct.pack('<' + 'I' * len(patched), *patched)
                replacement, errors = controller.BuildTargetShader('main', rd.ShaderEncoding.SPIRV,
                    binary, rd.ShaderCompileFlags(), rd.ShaderStage.Pixel)
                detail['replacement_error'] = errors
                controller.ReplaceResource(pipeline.GetShader(rd.ShaderStage.Pixel), replacement)
                controller.SetFrameEvent(event, True)
                pipeline = controller.GetPipelineState()
            for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel, rd.ShaderStage.Compute):
                reflection = pipeline.GetShaderReflection(stage)
                if reflection is None:
                    continue
                shader = {'stage': str(stage), 'shader': str(pipeline.GetShader(stage)),
                    'constantBlocks': [str(x) for x in reflection.constantBlocks],
                    'reflection_members': dir(reflection)}
                try:
                    disassembly = controller.DisassembleShader(pipeline.GetComputePipelineObject() if stage == rd.ShaderStage.Compute else pipeline.GetGraphicsPipelineObject(), reflection, '')
                    (out / f'event-{event}-{stage}.txt').write_text(disassembly)
                except Exception as error:
                    shader['disassembly_error'] = str(error)
                detail['stages'].append(shader)
                if request.get('constants'):
                    shader['debug_info'] = fields(reflection.debugInfo)
                    shader['blocks'] = fields(pipeline.GetConstantBlocks(stage))
                    shader['resources'] = fields(pipeline.GetReadOnlyResources(stage))
                    shader['writable'] = fields(pipeline.GetReadWriteResources(stage))
                    shader['reflection_blocks'] = fields(reflection.constantBlocks)
                    shader['constant_docs'] = controller.GetCBufferVariableContents.__doc__
                    shader['get_block_docs'] = pipeline.GetConstantBlock.__doc__
                    shader['values'] = []
                    for index, block in enumerate(reflection.constantBlocks):
                        used = pipeline.GetConstantBlock(stage, index, 0)
                        desc = used.descriptor
                        variables = controller.GetCBufferVariableContents(
                            pipeline.GetComputePipelineObject() if stage == rd.ShaderStage.Compute else pipeline.GetGraphicsPipelineObject(),
                            pipeline.GetShader(stage), stage, pipeline.GetShaderEntryPoint(stage),
                            index, desc.resource, desc.byteOffset, desc.byteSize)
                        shader['values'].append({'block': block.name, 'data': fields(variables)})
            if request.get('constants'):
                detail['pushconsts'] = fields(controller.GetVulkanPipelineState().pushconsts)
                detail['raster'] = fields(controller.GetVulkanPipelineState().rasterizer)
                detail['viewports'] = fields(controller.GetVulkanPipelineState().viewportScissor)
            if request.get('debug_pixel'):
                x, y = request['debug_pixel']
                trace = controller.DebugPixel(x, y, rd.DebugPixelInputs())
                debug = {'inputs': fields(trace.inputs), 'debugger': str(trace.debugger)}
                if trace.debugger:
                    debug['states'] = []
                    for step in range(100):
                        states = controller.ContinueDebug(trace.debugger)
                        if not states:
                            break
                        debug['states'].extend(fields(states))
                (out / f'event-{event}-debug.json').write_text(json.dumps(debug, indent=2))
                controller.FreeTrace(trace)
            if request.get('mesh'):
                mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
                detail['mesh'] = fields(mesh)
                (out / f'event-{event}-vertices.bin').write_bytes(bytes(
                    controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)))
            for texture in textures:
                texture_id = numeric(texture.resourceId)
                if texture_ids is not None:
                    if texture_id not in texture_ids:
                        continue
                elif not (texture.width in (1280, 640, 320, 160, 80) and texture.height in (720, 360, 180, 90, 45)):
                    continue
                minimum, maximum = controller.GetMinMax(texture.resourceId, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                detail[str(texture_id)] = {'min': list(minimum.floatValue), 'max': list(maximum.floatValue)}
                save = rd.TextureSave()
                save.resourceId = texture.resourceId
                save.destType = rd.FileType.PNG
                save.mip = 0
                save.slice.sliceIndex = 0
                controller.SaveTexture(save, str(out / f'event-{event}-texture-{texture_id}.png'))
                if request.get('raw_textures'):
                    (out / f'event-{event}-texture-{texture_id}.bin').write_bytes(bytes(
                        controller.GetTextureData(texture.resourceId, rd.Subresource(0, 0, 0))))
            for buffer_id in request.get('buffers', []):
                buf = next(x for x in controller.GetBuffers() if numeric(x.resourceId) == buffer_id)
                (out / f'event-{event}-buffer-{buffer_id}.bin').write_bytes(bytes(controller.GetBufferData(buf.resourceId, 0, 0)))
            for buffer_id, offset, length in request.get('buffer_ranges', []):
                buf = next(x for x in controller.GetBuffers() if numeric(x.resourceId) == buffer_id)
                (out / f'event-{event}-buffer-{buffer_id}-offset-{offset:x}.bin').write_bytes(bytes(
                    controller.GetBufferData(buf.resourceId, offset, length)))
            result['events'].append(detail)
        result['pipeline_methods'] = dir(controller.GetPipelineState())
        result['controller_methods'] = dir(controller)
        result['vulkan_pipeline_members'] = dir(controller.GetVulkanPipelineState())
        (out / 'capture-inspection.json').write_text(json.dumps(result, indent=2))
    finally:
        controller.Shutdown()
        cap.Shutdown()


code = 0
try:
    run()
except Exception:
    (out / 'capture-error.txt').write_text(traceback.format_exc())
    code = 1
os._exit(code)
