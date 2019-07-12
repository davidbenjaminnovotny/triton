from __future__ import print_function
import argparse
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torchvision import datasets, transforms
import triton
from torch.utils.cpp_extension import load
from torch.distributions import categorical

shift_cuda = load(
    'shift_cuda', ['/home/philippe/development/shiftnet/kernels/shift_cuda.cpp',
                   '/home/philippe/development/shiftnet/kernels/shift_cuda_kernel.cu'], extra_cflags=['-O3'])

class shift(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x, shift):
        ctx.save_for_backward(shift)
        return shift_cuda.forward(x, shift)

    @staticmethod
    def backward(ctx, grad_output):
        shift, = ctx.saved_tensors
        grad_output = shift_cuda.backward(grad_output, shift)

        return grad_output, None


class Shift(nn.Module):
    def __init__(self, in_channels, kernel_size):
        super(Shift, self).__init__()
        self.channels = in_channels
        self.kernel_size = kernel_size
        if kernel_size == 3:
            p = torch.Tensor([0.3, 0.4, 0.3])
        elif kernel_size == 5:
            p = torch.Tensor([0.1, 0.25, 0.3, 0.25, 0.1])
        elif kernel_size == 7:
            p = torch.Tensor([0.075, 0.1, 0.175, 0.3, 0.175, 0.1, 0.075])
        elif kernel_size == 9:
            p = torch.Tensor([0.05, 0.075, 0.1, 0.175, 0.2, 0.175, 0.1, 0.075, 0.05])
        else:
            raise RuntimeError('Unsupported kernel size')
        shift_t = categorical.Categorical(p).sample((in_channels, 2)) - (kernel_size // 2)
        self.register_buffer('shift_t', shift_t.int())

    def forward(self, x):
        if x.is_cuda:
            return shift.apply(x, self.shift_t)
        else:
            print('Shift only supports GPU for now..')
            assert False

    def extra_repr(self):
        s = ('{channels}, kernel_size={kernel_size}')
        return s.format(**self.__dict__)


def ShiftConv2d(in_planes, out_planes, kernel_size=3, stride=1, groups=1, dilation=1):
    return nn.Sequential(
        Shift(in_planes, kernel_size),
        nn.Conv2d(in_planes, out_planes, kernel_size=1, stride=stride,
                  padding=0, groups=groups, bias=False)
    )


class NetReference(nn.Module):
    def __init__(self):
        super(NetReference, self).__init__()
        #self.conv1 = ShiftConv2d(1, 32, 3, 2)
        self.conv1 = triton.ShiftConv2d(1, 32, 3, 2)
        self.bn1 = nn.BatchNorm2d(32)
        self.conv2 = triton.ShiftConv2d(32, 32, 3, 2)
        #self.conv2 = ShiftConv2d(32, 32, 3, 2)
        self.bn2 = nn.BatchNorm2d(32)
        self.fc1 = nn.Linear(32*7*7, 500)
        self.fc2 = nn.Linear(500, 10)

    def forward(self, x):
        x = self.conv1(x)
        x = self.bn1(x)
        x = F.relu(x)
        x = self.conv2(x)
        x = self.bn2(x)
        x = F.relu(x)
        x = x.view(-1, 32*7*7)
        x = F.relu(self.fc1(x))
        x = self.fc2(x)
        return F.log_softmax(x, dim=1)

class NetTriton(nn.Module):
    def __init__(self):
        super(NetTriton, self).__init__()
        self.conv1 = triton.ShiftConv2d(1, 32, 3, 2)
        self.bn1 = triton.BatchNorm2d(32)
        self.conv2 = triton.ShiftConv2d(32, 64, 3, 2)
        self.bn2 = triton.BatchNorm2d(64)
        self.fc1 = nn.Linear(64*7*7, 500)
        self.fc2 = nn.Linear(500, 10)

    def forward(self, x):
        x = x.permute(1, 2, 3, 0).contiguous()
        x = self.conv1(x)
        x = self.bn1(x)
        x = F.relu(x)
        x = self.conv2(x)
        x = self.bn2(x)
        x = F.relu(x)
        x = x.permute(3, 0, 1, 2).contiguous()
        x = x.view(-1, 64*7*7)
        x = F.relu(self.fc1(x))
        x = self.fc2(x)
        return F.log_softmax(x, dim=1)

Net = NetReference()

def train(args, model, device, train_loader, optimizer, epoch):
    model.train()
    for batch_idx, (data, target) in enumerate(train_loader):
        data, target = data.to(device), target.to(device)
        optimizer.zero_grad()
        output = model(data)
        loss = F.nll_loss(output, target)
        loss.backward()
        optimizer.step()
        if batch_idx % args.log_interval == 0:
            print('Train Epoch: {} [{}/{} ({:.0f}%)]\tLoss: {:.6f}'.format(
                epoch, batch_idx * len(data), len(train_loader.dataset),
                100. * batch_idx / len(train_loader), loss.item()))

def test(args, model, device, test_loader):
    model.eval()
    test_loss = 0
    correct = 0
    with torch.no_grad():
        for data, target in test_loader:
            data, target = data.to(device), target.to(device)
            output = model(data)
            test_loss += F.nll_loss(output, target, reduction='sum').item() # sum up batch loss
            pred = output.argmax(dim=1, keepdim=True) # get the index of the max log-probability
            correct += pred.eq(target.view_as(pred)).sum().item()

    test_loss /= len(test_loader.dataset)

    print('\nTest set: Average loss: {:.4f}, Accuracy: {}/{} ({:.0f}%)\n'.format(
        test_loss, correct, len(test_loader.dataset),
        100. * correct / len(test_loader.dataset)))

def main():
    # Training settings
    parser = argparse.ArgumentParser(description='PyTorch MNIST Example')
    parser.add_argument('--batch-size', type=int, default=64, metavar='N',
                        help='input batch size for training (default: 64)')
    parser.add_argument('--test-batch-size', type=int, default=1000, metavar='N',
                        help='input batch size for testing (default: 1000)')
    parser.add_argument('--epochs', type=int, default=10, metavar='N',
                        help='number of epochs to train (default: 10)')
    parser.add_argument('--lr', type=float, default=0.01, metavar='LR',
                        help='learning rate (default: 0.01)')
    parser.add_argument('--momentum', type=float, default=0.5, metavar='M',
                        help='SGD momentum (default: 0.5)')
    parser.add_argument('--no-cuda', action='store_true', default=False,
                        help='disables CUDA training')
    parser.add_argument('--seed', type=int, default=1, metavar='S',
                        help='random seed (default: 1)')
    parser.add_argument('--log-interval', type=int, default=10, metavar='N',
                        help='how many batches to wait before logging training status')

    parser.add_argument('--save-model', action='store_true', default=False,
                        help='For Saving the current Model')
    args = parser.parse_args()
    use_cuda = not args.no_cuda and torch.cuda.is_available()

    torch.manual_seed(args.seed)

    device = torch.device("cuda" if use_cuda else "cpu")

    kwargs = {'num_workers': 1, 'pin_memory': True} if use_cuda else {}
    train_loader = torch.utils.data.DataLoader(
        datasets.MNIST('../data', train=True, download=True,
                       transform=transforms.Compose([
                           transforms.ToTensor(),
                           transforms.Normalize((0.1307,), (0.3081,))
                       ])),
        batch_size=args.batch_size, shuffle=True, **kwargs)
    test_loader = torch.utils.data.DataLoader(
        datasets.MNIST('../data', train=False, transform=transforms.Compose([
                           transforms.ToTensor(),
                           transforms.Normalize((0.1307,), (0.3081,))
                       ])),
        batch_size=args.test_batch_size, shuffle=True, **kwargs)


    model = Net.to(device)
    optimizer = optim.SGD(model.parameters(), lr=args.lr, momentum=args.momentum)

    for epoch in range(1, args.epochs + 1):
        train(args, model, device, train_loader, optimizer, epoch)
        test(args, model, device, test_loader)

    if (args.save_model):
        torch.save(model.state_dict(),"mnist_cnn.pt")

main()
