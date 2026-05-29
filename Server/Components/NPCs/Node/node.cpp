/*
 *  This Source Code Form is subject to the terms of the Mozilla Public License,
 *  v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *  obtain one at http://mozilla.org/MPL/2.0/.
 *
 *  The original code is copyright (c) 2025, open.mp team and contributors.
 */

#include "node.hpp"
#include "../NPC/npc.hpp"
#include <random>
#include <ghc/filesystem.hpp>
#include <httplib.h>

NPCNode::NPCNode(int nodeId)
	: nodeId_(nodeId)
	, initialized_(false)
	, currentPointId_(0)
	, currentLinkId_(0)
{
	nodeHeader_ = {};
}

NPCNode::~NPCNode()
{
}

bool NPCNode::initialize(ICore* core)
{
	if (nodeId_ < 0 || nodeId_ >= 64)
	{
		return false;
	}

	std::string filePath = "scriptfiles/NPCs/nodes/NODES" + std::to_string(nodeId_) + ".DAT";

	if (!ghc::filesystem::exists(filePath))
	{
		std::string dirPath = "scriptfiles/NPCs/nodes";
		if (!ghc::filesystem::exists(dirPath))
		{
			ghc::filesystem::create_directories(dirPath);
		}

		std::string url = "assets.open.mp";
		std::string path = "/npc_nodes/NODES" + std::to_string(nodeId_) + ".DAT";

		httplib::Client client(url);
		client.set_connection_timeout(10, 0);
		client.set_read_timeout(30, 0);

		auto result = client.Get(path.c_str());
		if (result && result->status == 200)
		{
			std::ofstream outFile(filePath, std::ios::binary);
			if (outFile.is_open())
			{
				outFile.write(result->body.c_str(), result->body.size());
				outFile.close();
				core->logLn(LogLevel::Message, "[NPCs] Downloaded node file: NODES%d.DAT", nodeId_);
			}
			else
			{
				core->logLn(LogLevel::Warning, "[NPCs] Failed to save downloaded node file: NODES%d.DAT", nodeId_);
				core->logLn(LogLevel::Message, "[NPCs] Download the package manually from https://assets.open.mp/npc_nodes/NODES.zip and extract the contents in `scriptfiles/NPCs/nodes/NODES`");
				return false;
			}
		}
		else
		{
			core->logLn(LogLevel::Warning, "[NPCs] Failed to download node file: NODES%d.DAT (HTTP status: %d)", nodeId_, result ? result->status : -1);
			core->logLn(LogLevel::Message, "[NPCs] Download the package manually from https://assets.open.mp/npc_nodes/NODES.zip and extract the contents in `scriptfiles/NPCs/nodes/NODES`");
			return false;
		}
	}

	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open())
	{
		return false;
	}

	file.seekg(0, std::ios::end);
	std::streamsize fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	if (fileSize == 0)
	{
		return false;
	}

	if (!file.read(reinterpret_cast<char*>(&nodeHeader_), sizeof(NodeHeader)))
	{
		return false;
	}

	uint32_t totalPathNodes = nodeHeader_.vehicleNodesNumber + nodeHeader_.pedNodesNumber;
	pathNodes_.resize(totalPathNodes);
	if (totalPathNodes > 0)
	{
		if (!file.read(reinterpret_cast<char*>(pathNodes_.data()), totalPathNodes * sizeof(PathNode)))
		{
			return false;
		}
	}

	naviNodes_.resize(nodeHeader_.naviNodesNumber);
	if (nodeHeader_.naviNodesNumber > 0)
	{
		if (!file.read(reinterpret_cast<char*>(naviNodes_.data()), nodeHeader_.naviNodesNumber * sizeof(NaviNode)))
		{
			return false;
		}
	}

	linkNodes_.resize(nodeHeader_.linksNumber);
	if (nodeHeader_.linksNumber > 0)
	{
		if (!file.read(reinterpret_cast<char*>(linkNodes_.data()), nodeHeader_.linksNumber * sizeof(LinkNode)))
		{
			return false;
		}
	}

	file.seekg(768, std::ios::cur);
	if (!file)
	{
		return false;
	}

	naviLinkNodes_.resize(nodeHeader_.linksNumber);
	if (nodeHeader_.linksNumber > 0)
	{
		if (!file.read(reinterpret_cast<char*>(naviLinkNodes_.data()), nodeHeader_.linksNumber * sizeof(uint16_t)))
		{
			return false;
		}
	}

	linkLengths_.resize(nodeHeader_.linksNumber);
	if (nodeHeader_.linksNumber > 0)
	{
		if (!file.read(reinterpret_cast<char*>(linkLengths_.data()), nodeHeader_.linksNumber * sizeof(uint8_t)))
		{
			return false;
		}
	}

	intersectionFlags_.resize(nodeHeader_.linksNumber);
	if (nodeHeader_.linksNumber > 0)
	{
		if (!file.read(reinterpret_cast<char*>(intersectionFlags_.data()), nodeHeader_.linksNumber * sizeof(uint8_t)))
		{
			return false;
		}
	}

	file.close();

	if (!pathNodes_.empty())
	{
		currentPointId_ = 0;
	}

	initialized_ = true;
	return true;
}

int NPCNode::getLinkLaneCount(uint16_t linkId, uint16_t fromPointId) const
{
	if (!initialized_ || fromPointId >= pathNodes_.size() || linkId >= linkNodes_.size() || linkId >= naviLinkNodes_.size())
	{
		return -1;
	}

	const uint16_t naviLink = naviLinkNodes_[linkId];
	const uint16_t naviAreaId = static_cast<uint16_t>((naviLink >> 10) & 0x3F);
	const uint16_t naviNodeId = static_cast<uint16_t>(naviLink & 0x03FF);
	if (naviAreaId != nodeId_ || naviNodeId >= naviNodes_.size())
	{
		return -1;
	}

	const LinkNode& linkNode = linkNodes_[linkId];
	if (linkNode.areaId != nodeId_ || linkNode.nodeId >= pathNodes_.size())
	{
		return -1;
	}

	const NaviNode& naviNode = naviNodes_[naviNodeId];
	Vector2 naviDirection(static_cast<float>(static_cast<int8_t>(naviNode.directionX)) / 100.0f, static_cast<float>(static_cast<int8_t>(naviNode.directionY)) / 100.0f);
	float directionLength = glm::length(naviDirection);
	if (directionLength <= 0.0001f)
	{
		return -1;
	}
	naviDirection /= directionLength;

	const PathNode& fromNode = pathNodes_[fromPointId];
	const PathNode& targetNode = pathNodes_[linkNode.nodeId];
	Vector2 segmentDirection(
		static_cast<float>(targetNode.positionX - fromNode.positionX),
		static_cast<float>(targetNode.positionY - fromNode.positionY));
	float segmentLength = glm::length(segmentDirection);
	if (segmentLength <= 0.0001f)
	{
		return -1;
	}
	segmentDirection /= segmentLength;

	const uint32_t naviFlags = naviNode.flags;
	const int leftLanes = static_cast<int>((naviFlags >> 8) & 0x7);
	const int rightLanes = static_cast<int>((naviFlags >> 11) & 0x7);
	return glm::dot(segmentDirection, naviDirection) >= 0.0f ? leftLanes : rightLanes;
}

bool NPCNode::isLaneAwareDriveLinkAllowed(uint16_t linkId, uint16_t fromPointId) const
{
	return getLinkLaneCount(linkId, fromPointId) > 0;
}

bool NPCNode::selectLink(NPC* npc, uint16_t pointId, uint16_t lastPoint, bool laneAwareDrive, uint16_t& selectedLinkId)
{
	if (!setPoint(pointId))
	{
		return false;
	}

	const uint16_t startLink = getLinkId();
	const uint16_t linkCount = getLinkCount();
	if (startLink >= linkNodes_.size())
	{
		return false;
	}

	uint16_t laneCandidates[16];
	uint8_t laneCandidateCount = 0;
	uint16_t laneBacktrackCandidates[16];
	uint8_t laneBacktrackCount = 0;
	uint16_t fallbackCandidates[16];
	uint8_t fallbackCount = 0;
	uint16_t fallbackBacktrackCandidates[16];
	uint8_t fallbackBacktrackCount = 0;
	const uint16_t effectiveLinkCount = linkCount > 0 ? linkCount : 1;

	for (uint16_t i = 0; i < effectiveLinkCount && i < 16; i++)
	{
		const uint16_t linkId = startLink + i;
		if (linkId >= linkNodes_.size())
		{
			continue;
		}

		if (npc && !npc->canUseNodeLink(nodeId_, pointId, linkId, linkNodes_[linkId].areaId, linkNodes_[linkId].nodeId))
		{
			continue;
		}

		const bool backtracks = linkNodes_[linkId].nodeId == lastPoint && linkCount > 1;
		const int laneCount = laneAwareDrive ? getLinkLaneCount(linkId, pointId) : -1;
		const bool laneAllowed = !laneAwareDrive || laneCount > 0;
		const bool laneUnknown = laneAwareDrive && laneCount < 0;
		if (laneAllowed)
		{
			if (backtracks)
			{
				laneBacktrackCandidates[laneBacktrackCount++] = linkId;
			}
			else
			{
				laneCandidates[laneCandidateCount++] = linkId;
			}
			continue;
		}

		if (!laneUnknown)
		{
			continue;
		}

		if (backtracks)
		{
			fallbackBacktrackCandidates[fallbackBacktrackCount++] = linkId;
		}
		else
		{
			fallbackCandidates[fallbackCount++] = linkId;
		}
	}

	if (laneCandidateCount > 0)
	{
		selectedLinkId = laneCandidates[rand() % laneCandidateCount];
		return setLink(selectedLinkId);
	}

	if (laneBacktrackCount > 0)
	{
		selectedLinkId = laneBacktrackCandidates[rand() % laneBacktrackCount];
		return setLink(selectedLinkId);
	}

	if (fallbackCount > 0)
	{
		selectedLinkId = fallbackCandidates[rand() % fallbackCount];
		return setLink(selectedLinkId);
	}

	if (fallbackBacktrackCount > 0)
	{
		selectedLinkId = fallbackBacktrackCandidates[rand() % fallbackBacktrackCount];
		return setLink(selectedLinkId);
	}

	return false;
}

uint16_t NPCNode::process(NPC* npc, uint16_t pointId, uint16_t lastPoint, bool laneAwareDrive, uint16_t& currentLinkId)
{
	if (!initialized_)
	{
		return 0xFFFE;
	}

	uint16_t linkId = 0;
	if (!selectLink(npc, pointId, lastPoint, laneAwareDrive, linkId))
	{
		return 0xFFFE;
	}

	const LinkNode& currentLink = linkNodes_[linkId];
	currentLinkId = linkId;

	if (currentLink.areaId != nodeId_)
	{
		if (currentLink.areaId != 65535)
		{
			currentLinkId_ = linkId;
			return 0xFFFF;
		}
		else
		{
			return 0xFFFE;
		}
	}
	else
	{
		currentLinkId_ = linkId;
		npc->updateNodePoint(currentLink.nodeId);
		return currentLink.nodeId;
	}
}

uint16_t NPCNode::processNodeChange(NPC* npc, uint16_t targetPointId)
{
	if (!initialized_ || targetPointId >= pathNodes_.size())
	{
		return 0xFFFE;
	}

	currentPointId_ = targetPointId;
	npc->updateNodePoint(targetPointId);
	return targetPointId;
}

Vector3 NPCNode::getPosition()
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
	{
		return Vector3(0.0f, 0.0f, 0.0f);
	}

	const PathNode& pathNode = pathNodes_[currentPointId_];
	Vector3 normalPosition = Vector3(
		static_cast<float>(pathNode.positionX) / 8.0f,
		static_cast<float>(pathNode.positionY) / 8.0f,
		static_cast<float>(pathNode.positionZ) / 8.0f + 1.2f);
	return normalPosition;
}

Vector3 NPCNode::getLaneAwarePosition(uint16_t fromPointId) const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
	{
		return Vector3(0.0f, 0.0f, 0.0f);
	}

	const PathNode& targetNode = pathNodes_[currentPointId_];
	Vector3 position = Vector3(
		static_cast<float>(targetNode.positionX) / 8.0f,
		static_cast<float>(targetNode.positionY) / 8.0f,
		static_cast<float>(targetNode.positionZ) / 8.0f + 1.2f);

	if (fromPointId >= pathNodes_.size() || currentLinkId_ >= naviLinkNodes_.size())
	{
		return position;
	}

	const uint16_t naviLink = naviLinkNodes_[currentLinkId_];
	const uint16_t naviAreaId = static_cast<uint16_t>((naviLink >> 10) & 0x3F);
	const uint16_t naviNodeId = static_cast<uint16_t>(naviLink & 0x03FF);
	if (naviAreaId != nodeId_ || naviNodeId >= naviNodes_.size())
	{
		return position;
	}

	const NaviNode& naviNode = naviNodes_[naviNodeId];
	position.x = static_cast<float>(naviNode.positionX) / 8.0f;
	position.y = static_cast<float>(naviNode.positionY) / 8.0f;

	Vector2 naviDirection(static_cast<float>(static_cast<int8_t>(naviNode.directionX)) / 100.0f, static_cast<float>(static_cast<int8_t>(naviNode.directionY)) / 100.0f);
	float directionLength = glm::length(naviDirection);
	if (directionLength <= 0.0001f)
	{
		return position;
	}
	naviDirection /= directionLength;

	const PathNode& fromNode = pathNodes_[fromPointId];
	Vector2 segmentDirection(
		static_cast<float>(targetNode.positionX - fromNode.positionX),
		static_cast<float>(targetNode.positionY - fromNode.positionY));
	float segmentLength = glm::length(segmentDirection);
	if (segmentLength <= 0.0001f)
	{
		return position;
	}
	segmentDirection /= segmentLength;

	const bool forward = glm::dot(segmentDirection, naviDirection) >= 0.0f;
	const uint32_t naviFlags = naviNode.flags;
	const int leftLanes = static_cast<int>((naviFlags >> 8) & 0x7);
	const int rightLanes = static_cast<int>((naviFlags >> 11) & 0x7);
	const int laneCount = forward ? leftLanes : rightLanes;
	if (laneCount <= 0)
	{
		return position;
	}

	const int widthValue = static_cast<int>(naviFlags & 0xFF);
	const Vector2 rightOfNavi(naviDirection.y, -naviDirection.x);
	const float side = forward ? 1.0f : -1.0f;
	const float laneCenter = 2.7f + static_cast<float>(widthValue) / 16.0f;

	const Vector2 laneOffset = rightOfNavi * side * laneCenter;
	position.x += laneOffset.x;
	position.y += laneOffset.y;
	return position;
}

int NPCNode::getNodesNumber() const
{
	return initialized_ ? nodeHeader_.nodesNumber : 0;
}

void NPCNode::getHeaderInfo(uint32_t& vehicleNodes, uint32_t& pedNodes, uint32_t& naviNodes) const
{
	if (initialized_)
	{
		vehicleNodes = nodeHeader_.vehicleNodesNumber;
		pedNodes = nodeHeader_.pedNodesNumber;
		naviNodes = nodeHeader_.naviNodesNumber;
	}
	else
	{
		vehicleNodes = pedNodes = naviNodes = 0;
	}
}

bool NPCNode::getPathNodeData(uint16_t pointId, NPCPathNodeData& data) const
{
	if (!initialized_ || pointId >= pathNodes_.size())
	{
		data = NPCPathNodeData();
		return false;
	}

	const PathNode& pathNode = pathNodes_[pointId];
	data.position = Vector3(
		static_cast<float>(pathNode.positionX) / 8.0f,
		static_cast<float>(pathNode.positionY) / 8.0f,
		static_cast<float>(pathNode.positionZ) / 8.0f + 1.2f);
	data.linkId = pathNode.linkId;
	data.areaId = pathNode.areaId;
	data.nodeId = pathNode.nodeId;
	data.pathWidth = pathNode.pathWidth;
	data.floodFill = pathNode.nodeType;
	data.flags = pathNode.flags;
	return true;
}

bool NPCNode::getNaviNodeData(uint16_t naviId, NPCNaviNodeData& data) const
{
	if (!initialized_ || naviId >= naviNodes_.size())
	{
		data = NPCNaviNodeData();
		return false;
	}

	const NaviNode& naviNode = naviNodes_[naviId];
	data.position = Vector2(
		static_cast<float>(naviNode.positionX) / 8.0f,
		static_cast<float>(naviNode.positionY) / 8.0f);
	data.areaId = naviNode.areaId;
	data.nodeId = naviNode.nodeId;
	data.directionX = static_cast<int8_t>(naviNode.directionX);
	data.directionY = static_cast<int8_t>(naviNode.directionY);
	data.flags = naviNode.flags;
	return true;
}

int NPCNode::getLinkCountTotal() const
{
	return initialized_ ? static_cast<int>(linkNodes_.size()) : 0;
}

bool NPCNode::getLinkData(uint16_t linkId, NPCNodeLinkData& data) const
{
	if (!initialized_ || linkId >= linkNodes_.size())
	{
		data = NPCNodeLinkData();
		return false;
	}

	const LinkNode& linkNode = linkNodes_[linkId];
	const uint16_t naviLink = linkId < naviLinkNodes_.size() ? naviLinkNodes_[linkId] : 0;

	data.areaId = linkNode.areaId;
	data.nodeId = linkNode.nodeId;
	data.naviAreaId = static_cast<uint16_t>((naviLink >> 10) & 0x3F);
	data.naviNodeId = static_cast<uint16_t>(naviLink & 0x03FF);
	data.length = linkId < linkLengths_.size() ? linkLengths_[linkId] : 0;
	data.intersectionFlags = linkId < intersectionFlags_.size() ? intersectionFlags_[linkId] : 0;
	return true;
}

int NPCNode::getNodeId() const
{
	return nodeId_;
}

uint16_t NPCNode::getLinkId() const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
		return 0;
	return pathNodes_[currentPointId_].linkId;
}

uint16_t NPCNode::getAreaId() const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
		return 0;
	return pathNodes_[currentPointId_].areaId;
}

uint16_t NPCNode::getPointId() const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
		return 0;
	return pathNodes_[currentPointId_].nodeId;
}

uint16_t NPCNode::getLinkCount() const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
		return 0;
	return static_cast<uint16_t>(pathNodes_[currentPointId_].flags & 0xF);
}

uint8_t NPCNode::getPathWidth() const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
		return 0;
	return pathNodes_[currentPointId_].pathWidth;
}

uint8_t NPCNode::getNodeType() const
{
	if (!initialized_ || currentPointId_ >= pathNodes_.size())
		return 0;
	return pathNodes_[currentPointId_].nodeType;
}

uint16_t NPCNode::getLinkPoint() const
{
	if (!initialized_ || currentLinkId_ >= linkNodes_.size())
		return 0;
	return linkNodes_[currentLinkId_].nodeId;
}

uint16_t NPCNode::getLastLinkTargetNodeId() const
{
	if (!initialized_ || currentLinkId_ >= linkNodes_.size())
		return 0;
	return linkNodes_[currentLinkId_].areaId;
}

uint16_t NPCNode::getLastLinkTargetPointId() const
{
	if (!initialized_ || currentLinkId_ >= linkNodes_.size())
		return 0;
	return linkNodes_[currentLinkId_].nodeId;
}

bool NPCNode::setLink(uint16_t linkId)
{
	if (!initialized_ || linkId >= linkNodes_.size())
	{
		return false;
	}

	currentLinkId_ = linkId;
	return true;
}

bool NPCNode::setPoint(uint16_t pointId)
{
	if (!initialized_ || pointId >= pathNodes_.size())
	{
		return false;
	}

	currentPointId_ = pointId;
	return true;
}
