#include "pch.h"
#include "SpinningCubeRenderer.h"
#include "Common\DirectXHelper.h"

using namespace HolographicMRCSample;
using namespace Concurrency;
using namespace DirectX;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI::Input::Spatial;

// Loads vertex and pixel shaders from files and instantiates the cube geometry.
SpinningCubeRenderer::SpinningCubeRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources) :
    m_deviceResources(deviceResources)
{
    CreateDeviceDependentResources();
}

// This function uses a SpatialPointerPose to position the world-locked hologram
// two meters in front of the user's heading.
void SpinningCubeRenderer::PositionHologram(SpatialPointerPose^ pointerPose)
{
    if (pointerPose != nullptr)
    {
        // Get the gaze direction relative to the given coordinate system.
        const float3 headPosition    = pointerPose->Head->Position;
        const float3 headDirection   = pointerPose->Head->ForwardDirection;

        // The hologram is positioned two meters along the user's gaze direction.
        static const float distanceFromUser = 2.0f; // meters
        const float3 gazeAtTwoMeters        = headPosition + (distanceFromUser * headDirection);

        // This will be used as the translation component of the hologram's
        // model transform.
        SetPosition(gazeAtTwoMeters);
    }
}

// Called once per frame. Rotates the cube, and calculates and sets the model matrix
// relative to the position transform indicated by hologramPositionTransform.
void SpinningCubeRenderer::Update(const DX::StepTimer& timer)
{
    // Rotate the cube.
    // Convert degrees to radians, then convert seconds to rotation angle.
    const float    radiansPerSecond = XMConvertToRadians(m_degreesPerSecond);
    const double   totalRotation    = timer.GetTotalSeconds() * radiansPerSecond;
    const float    radians          = static_cast<float>(fmod(totalRotation, XM_2PI));
    const XMMATRIX modelRotation    = XMMatrixRotationY(-radians);

    // Position the cube.
    const XMMATRIX modelTranslation = XMMatrixTranslationFromVector(XMLoadFloat3(&m_position));

    // Multiply to get the transform matrix.
    // Note that this transform does not enforce a particular coordinate system. The calling
    // class is responsible for rendering this content in a consistent manner.
    const XMMATRIX modelTransform   = XMMatrixMultiply(modelRotation, modelTranslation);

    // The view and projection matrices are provided by the system; they are associated
    // with holographic cameras, and updated on a per-camera basis.
    // Here, we provide the model transform for the sample hologram. The model transform
    // matrix is transposed to prepare it for the shader.
    XMStoreFloat4x4(&m_modelConstantBufferData.model, XMMatrixTranspose(modelTransform));

    // Loading is asynchronous. Resources must be created before they can be updated.
    if (!m_loadingComplete)
    {
        return;
    }

    // Use the D3D device context to update Direct3D device-based resources.
    const auto context = m_deviceResources->GetD3DDeviceContext();

    // Update the model transform buffer for the hologram.
    context->UpdateSubresource(
        m_modelConstantBuffer.Get(),
        0,
        nullptr,
        &m_modelConstantBufferData,
        0,
        0
        );
}

// Renders one frame using the vertex and pixel shaders.
// On devices that do not support the D3D11_FEATURE_D3D11_OPTIONS3::
// VPAndRTArrayIndexFromAnyShaderFeedingRasterizer optional feature,
// a pass-through geometry shader is also used to set the render 
// target array index.
void SpinningCubeRenderer::Render()
{
    // Loading is asynchronous. Resources must be created before drawing can occur.
    if (!m_loadingComplete)
    {
        return;
    }

    const auto context = m_deviceResources->GetD3DDeviceContext();

    // Each vertex is one instance of the VertexPositionColor struct.
    const UINT stride = sizeof(VertexPositionColor);
    const UINT offset = 0;
    context->IASetVertexBuffers(
        0,
        1,
        m_vertexBuffer.GetAddressOf(),
        &stride,
        &offset
        );
    context->IASetIndexBuffer(
        m_indexBuffer.Get(),
        DXGI_FORMAT_R16_UINT, // Each index is one 16-bit unsigned integer (short).
        0
        );
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(m_inputLayout.Get());

    // Attach the vertex shader.
    context->VSSetShader(
        m_vertexShader.Get(),
        nullptr,
        0
        );
    // Apply the model constant buffer to the vertex shader.
    context->VSSetConstantBuffers(
        0,
        1,
        m_modelConstantBuffer.GetAddressOf()
        );

    if (!m_usingVprtShaders)
    {
        // On devices that do not support the D3D11_FEATURE_D3D11_OPTIONS3::
        // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer optional feature,
        // a pass-through geometry shader is used to set the render target 
        // array index.
        context->GSSetShader(
            m_geometryShader.Get(),
            nullptr,
            0
            );
    }

    // Attach the pixel shader.
    context->PSSetShader(
        m_pixelShader.Get(),
        nullptr,
        0
        );

    // Draw the objects.
    context->DrawIndexedInstanced(
        m_indexCount,   // Index count per instance.
        2,              // Instance count.
        0,              // Start index location.
        0,              // Base vertex location.
        0               // Start instance location.
        );
}

void SpinningCubeRenderer::CreateDeviceDependentResources()
{
    m_usingVprtShaders = m_deviceResources->GetDeviceSupportsVprt();

    // On devices that do support the D3D11_FEATURE_D3D11_OPTIONS3::
    // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer optional feature
    // we can avoid using a pass-through geometry shader to set the render
    // target array index, thus avoiding any overhead that would be 
    // incurred by setting the geometry shader stage.

    m_vertexShader = m_deviceResources->GetVertexShader(m_usingVprtShaders ? DX::VertexShader_VPRT : DX::VertexShader_Simple);
    m_inputLayout = m_deviceResources->GetInputLayout(m_usingVprtShaders ? DX::VertexShader_VPRT : DX::VertexShader_Simple);
    m_pixelShader = m_deviceResources->GetPixelShader(DX::PixelShader_Simple);

    if (!m_usingVprtShaders)
    {
        m_geometryShader = m_deviceResources->GetGeometryShader(DX::GeometryShader_Simple);
    }

    const CD3D11_BUFFER_DESC constantBufferDesc(sizeof(ModelConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
    DX::ThrowIfFailed(
        m_deviceResources->GetD3DDevice()->CreateBuffer(
            &constantBufferDesc,
            nullptr,
            &m_modelConstantBuffer
        )
    );

    // Load mesh vertices. Each vertex has a position and a color.
    // Note that the cube size has changed from the default DirectX app
    // template. Windows Holographic is scaled in meters, so to draw the
    // cube at a comfortable size we made the cube width 0.2 m (20 cm).
    static const VertexPositionColor cubeVertices[] =
    {
        { XMFLOAT3(-0.1f, -0.1f, -0.1f), XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.1f, -0.1f,  0.1f), XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f) },
        { XMFLOAT3(-0.1f,  0.1f, -0.1f), XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.1f,  0.1f,  0.1f), XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f) },
        { XMFLOAT3( 0.1f, -0.1f, -0.1f), XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f) },
        { XMFLOAT3( 0.1f, -0.1f,  0.1f), XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f) },
        { XMFLOAT3( 0.1f,  0.1f, -0.1f), XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f) },
        { XMFLOAT3( 0.1f,  0.1f,  0.1f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
    };

    D3D11_SUBRESOURCE_DATA vertexBufferData = {0};
    vertexBufferData.pSysMem = cubeVertices;
    vertexBufferData.SysMemPitch = 0;
    vertexBufferData.SysMemSlicePitch = 0;
    const CD3D11_BUFFER_DESC vertexBufferDesc(sizeof(cubeVertices), D3D11_BIND_VERTEX_BUFFER);
    DX::ThrowIfFailed(
        m_deviceResources->GetD3DDevice()->CreateBuffer(
            &vertexBufferDesc,
            &vertexBufferData,
            &m_vertexBuffer
            )
        );

    // Load mesh indices. Each trio of indices represents
    // a triangle to be rendered on the screen.
    // For example: 2,1,0 means that the vertices with indexes
    // 2, 1, and 0 from the vertex buffer compose the
    // first triangle of this mesh.
    // Note that the winding order is clockwise by default.
    static const unsigned short cubeIndices [] =
    {
        2,1,0, // -x
        2,3,1,

        6,4,5, // +x
        6,5,7,

        0,1,5, // -y
        0,5,4,

        2,6,7, // +y
        2,7,3,

        0,4,6, // -z
        0,6,2,

        1,3,7, // +z
        1,7,5,
    };

    m_indexCount = ARRAYSIZE(cubeIndices);

    D3D11_SUBRESOURCE_DATA indexBufferData = {0};
    indexBufferData.pSysMem          = cubeIndices;
    indexBufferData.SysMemPitch      = 0;
    indexBufferData.SysMemSlicePitch = 0;
    const CD3D11_BUFFER_DESC indexBufferDesc(sizeof(cubeIndices), D3D11_BIND_INDEX_BUFFER);
    DX::ThrowIfFailed(
        m_deviceResources->GetD3DDevice()->CreateBuffer(
            &indexBufferDesc,
            &indexBufferData,
            &m_indexBuffer
            )
        );

    // Once the cube is loaded, the object is ready to be rendered.
    m_loadingComplete = true;
}

void SpinningCubeRenderer::ReleaseDeviceDependentResources()
{
    m_loadingComplete  = false;
    m_usingVprtShaders = false;
    m_vertexShader.Reset();
    m_inputLayout.Reset();
    m_pixelShader.Reset();
    m_geometryShader.Reset();
    m_modelConstantBuffer.Reset();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
}