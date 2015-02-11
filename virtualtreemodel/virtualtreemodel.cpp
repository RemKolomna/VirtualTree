#include "virtualtreemodel.h"

class VirtualModelInterfaceImpl: public VirtualModelInterface
{
public:
    VirtualModelInterfaceImpl(VirtualTreeModel &model);
    void beginUpdate() override;
    void endUpdate() override;
    void QueuedUpdate() override;
private:
    VirtualTreeModel &m_model;
};


VirtualModelInterfaceImpl::VirtualModelInterfaceImpl(VirtualTreeModel &model): m_model(model)
{

}

void VirtualModelInterfaceImpl::beginUpdate() {
    m_model.beginUpdate();
}

void VirtualModelInterfaceImpl::endUpdate() {
    m_model.endUpdate();
}

void VirtualModelInterfaceImpl::QueuedUpdate()
{
    m_model.QueuedUpdate();
}

typedef std::vector<std::unique_ptr<InternalNode>> InternalChildren;

// internal tree structure
class InternalNode
{
public:
  InternalNode(InternalNode *parent, void *obj, size_t index) : parent(parent), item(obj), parentIndex(index) {}
  InternalNode *parent;
  size_t parentIndex;
  void *item;
  InternalChildren children;
  bool hasChildrenQueryed = false;
  bool hasChildren = false;
  bool childInitialized = false;

  bool isInitialized(VirtualModelAdapter *adapter)
  {
      if (!childInitialized)
      {
          // if node children have not been added to model
          // but model asks node by hasChildren()
          // the only way to notify model about changes is to initialize node children
          if (hasChildrenQueryed) {
              if ( hasChildren != (adapter->getItemsCount(item) > 0) )
                  childInitialized = true;
          }
          return childInitialized;
      }
      else
        return true;
  }

  void loadChildren(VirtualModelAdapter *adapter)
  {
      if (!childInitialized)
      {
        if (adapter->hasItems(item))
        {
          int childCount = adapter->getItemsCount(item);
          for (int k = 0; k < childCount; ++k)
            children.emplace_back(new InternalNode(this, adapter->getItem(item, k), k));
        }
        childInitialized = true;
      }
  }

  int childCount(VirtualModelAdapter *adapter)
  {
    loadChildren(adapter);
    return static_cast<int>(children.size());
  }

  void eraseChildren(InternalChildren::const_iterator begin, InternalChildren::const_iterator end)
  {
    size_t curParent = begin->get()->parentIndex;
    auto newEnd = children.erase(begin, end);
    for (auto it = newEnd; it != children.end(); ++it)
      it->get()->parentIndex = curParent++;
  }

  void insertedChildren(size_t lastIndex)
  {
    for (auto index = lastIndex; index < children.size(); ++index)
      children[index]->parentIndex = index;
  }
};

VirtualTreeModel::VirtualTreeModel(VirtualModelAdapter *adapter, QObject *parent) :
    QAbstractItemModel(parent), m_syncing(false), m_updating(0),
    m_root(std::make_unique<InternalNode>(nullptr, nullptr, 0)), m_adapter(adapter)
{
    m_intf.reset(new VirtualModelInterfaceImpl(*this));
    adapter->setModel(m_intf.get());
    syncTree();
}

void VirtualTreeModel::syncNodeList(InternalNode &node, void *parent)
{
  InternalChildren &nodes = node.children;
  int srcStart = 0;
  int srcCur = srcStart;
  int destStart = 0;
  
  auto index = getIndex(node);
  while (srcCur <= nodes.size())
  {
    bool finishing = srcCur >= nodes.size();
    int destCur = 0;
    InternalNode *curNode = nullptr;
    if (!finishing) {
      curNode = nodes[srcCur].get();
      destCur = m_adapter->indexOf(parent, curNode->item, destStart);
    }
    if (destCur >= 0)
    {
      // remove skipped source nodes
      if (srcCur > srcStart)
      {                
        beginRemoveRows(index, static_cast<int>(srcStart), static_cast<int>(srcCur) - 1);
        node.eraseChildren(nodes.begin() + srcStart, nodes.begin() + srcCur);        
        if (!finishing)
          srcCur = srcStart;                  
        endRemoveRows();        
      }
      srcStart = srcCur + 1;

      if (finishing)
        destCur = m_adapter->getItemsCount(parent);
      // insert skipped new nodes
      if (destCur > destStart)
      {
        int insertCount = destCur - destStart;
        beginInsertRows(index, static_cast<int>(srcCur), static_cast<int>(srcCur + insertCount) - 1);
        for (int i = 0, cur = srcCur; i < insertCount; i++, cur++)
        {
          void *obj = m_adapter->getItem(parent, destStart + i);
          auto newNode = new InternalNode(&node, obj, cur);
          // just add new node we shouldn't sync its children yet
          nodes.emplace(nodes.begin() + cur, newNode);          
        }
        node.insertedChildren(srcCur + insertCount);
        endInsertRows();

        srcCur += insertCount;
        destStart += static_cast<int>(insertCount);
      }
      destStart = destCur + 1;

      if (curNode && curNode->isInitialized(m_adapter))
      {
        syncNodeList(*curNode, curNode->item);
        srcStart = srcCur + 1;
      }
    }
    srcCur++;
  }
  node.childInitialized = true;
}

QVariant VirtualTreeModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid())
    return QVariant();

  if (m_updating > 0)
    return QVariant();

  void *item = getNode(index).item;
  return m_adapter->data(item, role);
}

QModelIndex VirtualTreeModel::index(int row, int column, const QModelIndex &parent) const
{
  InternalNode &parentItem = getNode(parent);
  if (row < parentItem.children.size())
  {
    InternalNode *childItem = parentItem.children.at(row).get();
    return getIndex(*childItem, column);
  }
  else
    return QModelIndex();  
}

QModelIndex VirtualTreeModel::parent(const QModelIndex &index) const
{
  if (!index.isValid())
    return QModelIndex();

  InternalNode &childItem = getNode(index);
  InternalNode *parentItem = childItem.parent;
  if (parentItem == nullptr || parentItem == m_root.get())
    return QModelIndex();
  return getIndex(*parentItem);
}

int VirtualTreeModel::rowCount(const QModelIndex &parent) const
{
  if (parent.isValid() && !this->parent(parent).isValid())
      parent.internalId();
  InternalNode &parentItem = getNode(parent);
  if (m_syncing)
    return static_cast<int>(parentItem.children.size());
  else
    // lazy children loading
    return parentItem.childCount(m_adapter);  
}

int VirtualTreeModel::columnCount(const QModelIndex &parent) const
{
  Q_UNUSED(parent);
  return 1;
}

InternalNode & VirtualTreeModel::getNode(const QModelIndex &index) const
{
  if (index.isValid())
    return *(InternalNode*)index.internalPointer();
  else
      return *m_root;
}

InternalNode *VirtualTreeModel::getItemNode(void *item) const
{
    void * parentItem = m_adapter->getItemParent(item);
    if (parentItem == item)
        return nullptr;
    if (parentItem == nullptr)
        return m_root.get();
    else
    {
        auto parentNode = getItemNode(parentItem);
        int index = m_adapter->indexOf(parentItem, item);
        if (index >= 0)
        {
            parentNode->loadChildren(m_adapter);
            return parentNode->children[index].get();
        }
        else
        {
            return nullptr;
        }
    }
}

QModelIndex VirtualTreeModel::getIndex(const InternalNode &node, int column) const
{
  if (&node == m_root.get())
    return QModelIndex();
  else
    return createIndex(static_cast<int>(node.parentIndex), column, quintptr(&node));
}

void VirtualTreeModel::doQueuedUpdate()
{
  endUpdate();
}

bool VirtualTreeModel::hasChildren(const QModelIndex &parent) const
{
  if (!parent.isValid())
    return true;
  InternalNode &item = getNode(parent);
  if (item.childInitialized)
  {
    return item.children.size() > 0;
  }
  else
  {
      item.hasChildrenQueryed = true;
      bool has = m_adapter->hasItems(item.item);
      item.hasChildren = has;
      return has;
  }
}

void *VirtualTreeModel::getItem(const QModelIndex &index) const
{
    return getNode(index).item;
}

QModelIndex VirtualTreeModel::getItemIndex(void *item) const
{
    auto node = getItemNode(item);
    if (node)
      return getIndex(*node);
    else
      return QModelIndex();
}

void VirtualTreeModel::beginUpdate()
{
  ++m_updating;
}

void VirtualTreeModel::endUpdate()
{ 
  if (m_updating == 1)
    syncTree();
  --m_updating;
  // force tree to repaint all nodes
  if (m_updating == 0)
    emit dataChanged(QModelIndex(), QModelIndex());
}

void VirtualTreeModel::QueuedUpdate()
{
    if (m_updating == 0)
    {
      beginUpdate();
      QMetaObject::invokeMethod(this, "doQueuedUpdate", Qt::QueuedConnection);
    }
}

void VirtualTreeModel::syncTree()
{
  m_syncing = true;
  syncNodeList(*m_root, nullptr);
  m_syncing = false;
}