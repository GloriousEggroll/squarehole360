// Exercise the renderer's GLSL transfer function on Vulkan, with independent
// ST2084 reference points, luminance/color checks and a monotonic grey ramp.
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "hdr_transfer_test.h"
#define VK_CHECK(call) do { auto result=(call); if(result != VK_SUCCESS) throw std::runtime_error(#call); } while(0)
struct Cleanup {
  std::vector<std::function<void()>> actions;
  ~Cleanup() { for(auto it=actions.rbegin();it!=actions.rend();++it) (*it)(); }
};
struct Sample { float rgb[4], parameters[4], result[4]; };
double DecodePQ(double pq) {
  const double p=std::pow(pq,32.0/2523);
  return 10000*std::pow(std::max(0.0,p-3424.0/4096)/(2413.0/128-2392.0/128*p),16384.0/2610);
}
void Near(double a,double b,double eps) { if(!std::isfinite(a)||std::abs(a-b)>eps) throw std::runtime_error("HDR reference mismatch: "+std::to_string(a)+" vs "+std::to_string(b)); }
int main() try {
  Cleanup cleanup;
  VkInstance instance; VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  VK_CHECK(vkCreateInstance(&ici,nullptr,&instance)); cleanup.actions.push_back([=]{vkDestroyInstance(instance,nullptr);});
  uint32_t count=0; VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
  std::vector<VkPhysicalDevice> gpus(count); VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,gpus.data()));
  VkPhysicalDevice gpu=VK_NULL_HANDLE; uint32_t family=0;
  for(auto candidate:gpus) {
    uint32_t n=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
    std::vector<VkQueueFamilyProperties> queues(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
    for(uint32_t i=0;i<n;++i) if(queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {gpu=candidate;family=i;break;}
    if(gpu)break;
  }
  if(!gpu)throw std::runtime_error("No Vulkan compute queue");
  float priority=1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
  VkDevice device; VK_CHECK(vkCreateDevice(gpu,&dci,nullptr,&device));cleanup.actions.push_back([=]{vkDestroyDevice(device,nullptr);});
  VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
  std::vector<Sample> samples={
    {{0,0,0,1},{100,1000,1,0},{}},
    {{1,1,1,1},{100,1000,0,0},{}},
    {{1,1,1,1},{100,1000,1,0},{}},
    {{1,1,1,1},{100,10000,1,0},{}},
    {{0.5,0.5,0.5,1},{100,100,0,0},{}},
    {{1,0,0,1},{100,100,0,0},{}},
    {{0,1,0,1},{100,100,0,0},{}},
    {{0,0,1,1},{100,100,0,0},{}},
  };
  for(int i=0;i<=100;++i){float v=i/100.0f;samples.push_back({{v,v,v,1},{203,427,1,0},{}});}
  VkDeviceSize bytes=samples.size()*sizeof(Sample);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=bytes;bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer buffer;VK_CHECK(vkCreateBuffer(device,&bci,nullptr,&buffer));
  VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
  VkPhysicalDeviceMemoryProperties props;vkGetPhysicalDeviceMemoryProperties(gpu,&props);
  uint32_t type=UINT32_MAX;for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
  if(type==UINT32_MAX)throw std::runtime_error("No coherent host memory");
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mai.allocationSize=req.size;mai.memoryTypeIndex=type;
  VkDeviceMemory memory;VK_CHECK(vkAllocateMemory(device,&mai,nullptr,&memory));
  cleanup.actions.push_back([=]{vkFreeMemory(device,memory,nullptr);});cleanup.actions.push_back([=]{vkDestroyBuffer(device,buffer,nullptr);});
  VK_CHECK(vkBindBufferMemory(device,buffer,memory,0));void* mapped;VK_CHECK(vkMapMemory(device,memory,0,bytes,0,&mapped));cleanup.actions.push_back([=]{vkUnmapMemory(device,memory);});std::memcpy(mapped,samples.data(),bytes);
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lci.bindingCount=1;lci.pBindings=&binding;
  VkDescriptorSetLayout layout;VK_CHECK(vkCreateDescriptorSetLayout(device,&lci,nullptr,&layout));cleanup.actions.push_back([=]{vkDestroyDescriptorSetLayout(device,layout,nullptr);});
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&layout;
  VkPipelineLayout pipeline_layout;VK_CHECK(vkCreatePipelineLayout(device,&plci,nullptr,&pipeline_layout));cleanup.actions.push_back([=]{vkDestroyPipelineLayout(device,pipeline_layout,nullptr);});
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=sizeof(hdr_transfer_test);smci.pCode=hdr_transfer_test;
  VkShaderModule shader;VK_CHECK(vkCreateShaderModule(device,&smci,nullptr,&shader));cleanup.actions.push_back([=]{vkDestroyShaderModule(device,shader,nullptr);});
  VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.layout=pipeline_layout;pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pci.stage.module=shader;pci.stage.pName="main";
  VkPipeline pipeline;VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));cleanup.actions.push_back([=]{vkDestroyPipeline(device,pipeline,nullptr);});
  VkDescriptorPoolSize poolsize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=1;dpci.pPoolSizes=&poolsize;
  VkDescriptorPool pool;VK_CHECK(vkCreateDescriptorPool(device,&dpci,nullptr,&pool));cleanup.actions.push_back([=]{vkDestroyDescriptorPool(device,pool,nullptr);});
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&layout;
  VkDescriptorSet set;VK_CHECK(vkAllocateDescriptorSets(device,&dsai,&set));VkDescriptorBufferInfo dbi{buffer,0,bytes};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&dbi;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpci.queueFamilyIndex=family;VkCommandPool command_pool;VK_CHECK(vkCreateCommandPool(device,&cpci,nullptr,&command_pool));cleanup.actions.push_back([=]{vkDestroyCommandPool(device,command_pool,nullptr);});
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbai.commandPool=command_pool;cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbai.commandBufferCount=1;VkCommandBuffer command;VK_CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(command,&cbi));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdDispatch(command,samples.size(),1,1);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);VK_CHECK(vkEndCommandBuffer(command));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;VK_CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_CHECK(vkQueueWaitIdle(queue));std::memcpy(samples.data(),mapped,bytes);
  for(int c=0;c<3;++c){Near(samples[0].result[c],0,2e-6);Near(samples[1].result[c],0.5080784215,2e-5);Near(samples[2].result[c],0.7518270962,2e-5);Near(samples[3].result[c],1,2e-5);Near(DecodePQ(samples[4].result[c]),21.40411405,0.005);}
  const double primaries[3][3]={{62.7404,6.9097,1.63916},{32.9282,91.954,8.80132},{4.33136,1.13612,89.5595}};
  for(int i=0;i<3;++i)for(int c=0;c<3;++c)Near(DecodePQ(samples[i+5].result[c]),primaries[i][c],0.015);
  for(size_t i=8;i<samples.size();++i){Near(samples[i].result[3],1,1e-6);for(int c=0;c<3;++c){if(i>8&&samples[i].result[c]<samples[i-1].result[c])throw std::runtime_error("Non-monotonic HDR ramp");if(DecodePQ(samples[i].result[c])>427.1)throw std::runtime_error("Peak exceeded");}}
  std::cout<<"PASS: "<<samples.size()<<" GPU HDR samples; PQ reference points, black, greys, primaries, peak limit, monotonic ramp\n";
  return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
