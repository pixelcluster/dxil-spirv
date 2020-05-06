Texture2D<float4> Tex[2] : register(t0);
Texture2D<float4> TexUnsized[] : register(t0, space1);

Texture2D<float4> TexSBT[] : register(t10, space15);
RWTexture2D<float4> UAVTexSBT[] : register(u10, space15);

struct CBVData { float4 v; float4 w; };

ConstantBuffer<CBVData> SBTCBV : register(b3, space15);
ConstantBuffer<CBVData> SBTCBVs[] : register(b4, space15);

ConstantBuffer<CBVData> SBTRootConstant : register(b0, space15);
ConstantBuffer<CBVData> SBTRootDescriptor : register(b2, space15);

SamplerState Samp : register(s3, space15);
SamplerState Samps[] : register(s4, space15);

struct Payload
{
	float4 color;
	int index;
};

[shader("miss")]
void RayMiss(inout Payload payload)
{
	payload.color = Tex[payload.index & 1].Load(int3(0, 0, 0));
	payload.color += TexUnsized[payload.index].Load(int3(0, 0, 0));
	payload.color += TexSBT[payload.index].Load(int3(0, 0, 0));
	payload.color += UAVTexSBT[payload.index].Load(int2(0, 0));
	payload.color += SBTCBV.v;
	payload.color += SBTCBVs[payload.index].v;
	payload.color += SBTRootConstant.v;
	payload.color += SBTRootConstant.w;
	payload.color += SBTRootDescriptor.w;

	payload.color += Tex[payload.index & 1].SampleLevel(Samp, 0.5.xx, 0.0);
	payload.color += TexUnsized[payload.index].SampleLevel(Samps[payload.index ^ 1], 0.5.xx, 0.0);
}
